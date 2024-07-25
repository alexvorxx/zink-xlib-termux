// Copyright © 2022 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::api::ShaderBin;
use crate::cfg::CFGBuilder;
use crate::ir::*;
use crate::sm50::ShaderModel50;
use crate::sm70::ShaderModel70;

use acorn::Acorn;
use nak_bindings::*;
use nak_runner::{Runner, CB0};
use std::str::FromStr;
use std::sync::OnceLock;

// from https://internals.rust-lang.org/t/discussion-on-offset-of/7440/2
macro_rules! offset_of {
    ($Struct:path, $field:ident) => {{
        // Using a separate function to minimize unhygienic hazards
        // (e.g. unsafety of #[repr(packed)] field borrows).
        // Uncomment `const` when `const fn`s can juggle pointers.

        // const
        fn offset() -> usize {
            let u = std::mem::MaybeUninit::<$Struct>::uninit();
            // Use pattern-matching to avoid accidentally going through Deref.
            let &$Struct { $field: ref f, .. } = unsafe { &*u.as_ptr() };
            let o =
                (f as *const _ as usize).wrapping_sub(&u as *const _ as usize);
            // Triple check that we are within `u` still.
            assert!((0..=std::mem::size_of_val(&u)).contains(&o));
            o
        }
        offset()
    }};
}

struct RunSingleton {
    sm: Box<dyn ShaderModel + Send + Sync>,
    run: Runner,
}

static RUN_SINGLETON: OnceLock<RunSingleton> = OnceLock::new();

impl RunSingleton {
    pub fn get() -> &'static RunSingleton {
        RUN_SINGLETON.get_or_init(|| {
            let dev_id = match std::env::var("NAK_TEST_DEVICE") {
                Ok(s) => Some(usize::from_str(&s).unwrap()),
                Err(_) => None,
            };

            let run = Runner::new(dev_id);
            let sm_nr = run.dev_info().sm;
            let sm: Box<dyn ShaderModel + Send + Sync> = if sm_nr >= 70 {
                Box::new(ShaderModel70::new(sm_nr))
            } else if sm_nr >= 50 {
                Box::new(ShaderModel50::new(sm_nr))
            } else {
                panic!("Unsupported shader model");
            };
            RunSingleton { sm, run }
        })
    }
}

const LOCAL_SIZE_X: u16 = 32;

pub struct TestShaderBuilder<'a> {
    sm: &'a dyn ShaderModel,
    alloc: SSAValueAllocator,
    b: InstrBuilder<'a>,
    start_block: BasicBlock,
    label: Label,
    data_addr: SSARef,
}

impl<'a> TestShaderBuilder<'a> {
    pub fn new(sm: &'a dyn ShaderModel) -> TestShaderBuilder {
        let mut alloc = SSAValueAllocator::new();
        let mut label_alloc = LabelAllocator::new();
        let mut b = SSAInstrBuilder::new(sm, &mut alloc);

        // Fill out the start block
        let lane = b.alloc_ssa(RegFile::GPR, 1);
        b.push_op(OpS2R {
            dst: lane.into(),
            idx: NAK_SV_LANE_ID,
        });

        let cta = b.alloc_ssa(RegFile::GPR, 1);
        b.push_op(OpS2R {
            dst: cta.into(),
            idx: NAK_SV_CTAID,
        });

        let invoc_id = b.alloc_ssa(RegFile::GPR, 1);
        b.push_op(OpIMad {
            dst: invoc_id.into(),
            srcs: [cta.into(), u32::from(LOCAL_SIZE_X).into(), lane.into()],
            signed: false,
        });

        let data_addr_lo = CBufRef {
            buf: CBuf::Binding(0),
            offset: offset_of!(CB0, data_addr_lo).try_into().unwrap(),
        };
        let data_addr_hi = CBufRef {
            buf: CBuf::Binding(0),
            offset: offset_of!(CB0, data_addr_hi).try_into().unwrap(),
        };
        let data_addr = b.alloc_ssa(RegFile::GPR, 2);
        b.copy_to(data_addr[0].into(), data_addr_lo.into());
        b.copy_to(data_addr[1].into(), data_addr_hi.into());

        let data_stride = CBufRef {
            buf: CBuf::Binding(0),
            offset: offset_of!(CB0, data_stride).try_into().unwrap(),
        };
        let invocations = CBufRef {
            buf: CBuf::Binding(0),
            offset: offset_of!(CB0, invocations).try_into().unwrap(),
        };

        let data_offset = SSARef::from([
            b.imul(invoc_id.into(), data_stride.into())[0],
            b.copy(0.into())[0],
        ]);
        let data_addr =
            b.iadd64(data_addr.into(), data_offset.into(), 0.into());

        // Finally, exit if we're OOB
        let oob = b.isetp(
            IntCmpType::U32,
            IntCmpOp::Ge,
            invoc_id.into(),
            invocations.into(),
        );
        b.predicate(oob[0].into()).push_op(OpExit {});

        let start_block = BasicBlock {
            label: label_alloc.alloc(),
            uniform: true,
            instrs: b.as_vec(),
        };

        TestShaderBuilder {
            sm,
            alloc: alloc,
            b: InstrBuilder::new(sm),
            start_block,
            label: label_alloc.alloc(),
            data_addr,
        }
    }

    pub fn ld_test_data(&mut self, offset: u16, mem_type: MemType) -> SSARef {
        let access = MemAccess {
            mem_type: mem_type,
            space: MemSpace::Global(MemAddrType::A64),
            order: MemOrder::Strong(MemScope::System),
            eviction_priority: MemEvictionPriority::Normal,
        };
        let comps: u8 = mem_type.bits().div_ceil(32).try_into().unwrap();
        let dst = self.alloc_ssa(RegFile::GPR, comps);
        self.push_op(OpLd {
            dst: dst.into(),
            addr: self.data_addr.into(),
            offset: offset.into(),
            access: access,
        });
        dst
    }

    pub fn st_test_data(
        &mut self,
        offset: u16,
        mem_type: MemType,
        data: SSARef,
    ) {
        let access = MemAccess {
            mem_type: mem_type,
            space: MemSpace::Global(MemAddrType::A64),
            order: MemOrder::Strong(MemScope::System),
            eviction_priority: MemEvictionPriority::Normal,
        };
        let comps: u8 = mem_type.bits().div_ceil(32).try_into().unwrap();
        assert!(data.comps() == comps);
        self.push_op(OpSt {
            addr: self.data_addr.into(),
            data: data.into(),
            offset: offset.into(),
            access: access,
        });
    }

    pub fn compile(mut self) -> Box<ShaderBin> {
        self.b.push_op(OpExit {});
        let block = BasicBlock {
            label: self.label,
            uniform: true,
            instrs: self.b.as_vec(),
        };

        let mut cfg = CFGBuilder::new();
        cfg.add_node(0, self.start_block);
        cfg.add_node(1, block);
        cfg.add_edge(0, 1);

        let f = Function {
            ssa_alloc: self.alloc,
            phi_alloc: PhiAllocator::new(),
            blocks: cfg.as_cfg(),
        };

        let cs_info = ComputeShaderInfo {
            local_size: [32, 1, 1],
            smem_size: 0,
        };
        let info = ShaderInfo {
            num_gprs: 0,
            num_control_barriers: 0,
            num_instrs: 0,
            slm_size: 0,
            uses_global_mem: true,
            writes_global_mem: true,
            uses_fp64: false,
            stage: ShaderStageInfo::Compute(cs_info),
            io: ShaderIoInfo::None,
        };
        let mut s = Shader {
            sm: self.sm,
            info: info,
            functions: vec![f],
        };

        // We do run a few passes
        s.opt_copy_prop();
        s.opt_dce();
        s.legalize();

        s.assign_regs();
        s.lower_par_copies();
        s.lower_copy_swap();
        s.calc_instr_deps();

        s.gather_info();
        s.remove_annotations();

        let code = self.sm.encode_shader(&s);
        Box::new(ShaderBin::new(self.sm, &s.info, None, code, ""))
    }
}

impl Builder for TestShaderBuilder<'_> {
    fn push_instr(&mut self, instr: Box<Instr>) -> &mut Instr {
        self.b.push_instr(instr)
    }

    fn sm(&self) -> u8 {
        self.b.sm()
    }
}

impl SSABuilder for TestShaderBuilder<'_> {
    fn alloc_ssa(&mut self, file: RegFile, comps: u8) -> SSARef {
        self.alloc.alloc_vec(file, comps)
    }
}

#[test]
fn test_sanity() {
    let run = RunSingleton::get();
    let b = TestShaderBuilder::new(run.sm.as_ref());
    let bin = b.compile();
    unsafe {
        run.run
            .run_raw(&bin, LOCAL_SIZE_X.into(), 0, std::ptr::null_mut(), 0)
            .unwrap();
    }
}

fn f32_eq(a: f32, b: f32) -> bool {
    if a.is_nan() && b.is_nan() {
        true
    } else if a.is_nan() || b.is_nan() {
        // If one is NaN but not the other, fail
        false
    } else {
        (a - b).abs() < 0.000001
    }
}

fn f64_eq(a: f64, b: f64) -> bool {
    if a.is_nan() && b.is_nan() {
        true
    } else if a.is_nan() || b.is_nan() {
        // If one is NaN but not the other, fail
        false
    } else {
        (a - b).abs() < 0.000001
    }
}

pub fn test_foldable_op_with(
    mut op: impl Foldable + Clone + Into<Op>,
    mut rand_u32: impl FnMut(usize) -> u32,
) {
    let run = RunSingleton::get();
    let mut b = TestShaderBuilder::new(run.sm.as_ref());

    let mut comps = 0_u16;
    let mut fold_src = Vec::new();
    let src_types = op.src_types();
    for (i, src) in op.srcs_as_mut_slice().iter_mut().enumerate() {
        match src_types[i] {
            SrcType::GPR
            | SrcType::ALU
            | SrcType::F16
            | SrcType::F16v2
            | SrcType::F32
            | SrcType::I32
            | SrcType::B32 => {
                let data = b.ld_test_data(comps * 4, MemType::B32);
                comps += 1;

                *src = data.into();
                fold_src.push(FoldData::U32(0));
            }
            SrcType::F64 => {
                todo!("Double ops aren't tested yet");
            }
            SrcType::Pred => {
                let data = b.ld_test_data(comps * 4, MemType::B32);
                comps += 1;

                let bit = b.lop2(LogicOp2::And, data.into(), 1.into());
                let pred = b.isetp(
                    IntCmpType::U32,
                    IntCmpOp::Ne,
                    bit.into(),
                    0.into(),
                );
                *src = pred.into();
                fold_src.push(FoldData::Pred(false));
            }
            typ => panic!("Can't auto-generate {typ:?} data"),
        }
    }
    let src_comps = usize::from(comps);

    let mut fold_dst = Vec::new();
    let dst_types = op.dst_types();
    for (i, dst) in op.dsts_as_mut_slice().iter_mut().enumerate() {
        match dst_types[i] {
            DstType::Pred => {
                *dst = b.alloc_ssa(RegFile::Pred, 1).into();
                fold_dst.push(FoldData::Pred(false));
            }
            DstType::GPR | DstType::F32 => {
                *dst = b.alloc_ssa(RegFile::GPR, 1).into();
                fold_dst.push(FoldData::U32(0));
            }
            DstType::F64 => {
                *dst = b.alloc_ssa(RegFile::GPR, 2).into();
                fold_dst.push(FoldData::Vec2([0, 0]));
            }
            typ => panic!("Can't auto-test {typ:?} data"),
        }
    }

    b.push_op(op.clone());
    let op = op; // Drop mutability

    for dst in op.dsts_as_slice() {
        let Dst::SSA(vec) = dst else {
            panic!("Should be an ssa value");
        };

        for ssa in &vec[..] {
            let u = match ssa.file() {
                RegFile::Pred => b.sel((*ssa).into(), 1.into(), 0.into()),
                RegFile::GPR => (*ssa).into(),
                file => panic!("Can't auto-test {file:?} data"),
            };
            b.st_test_data(comps * 4, MemType::B32, u);
            comps += 1;
        }
    }
    let comps = usize::from(comps); // Drop mutability
    let dst_comps = comps - src_comps;

    let bin = b.compile();

    // We're throwing random data at it here so the idea is that the number
    // of test cases we need to get good coverage is relative to the square
    // of the number of components.  For a big op like IAdd3X, this is going
    // to give us 2500 iterations.
    let invocations = src_comps * src_comps * 100;

    let mut data = Vec::new();
    for _ in 0..invocations {
        for (i, src) in op.srcs_as_slice().iter().enumerate() {
            let SrcRef::SSA(vec) = &src.src_ref else {
                panic!("Should be an ssa value");
            };

            for _ in 0..vec.comps() {
                data.push(rand_u32(i));
            }
        }
        for _ in 0..dst_comps {
            data.push(0_u32);
        }
    }
    debug_assert!(data.len() == invocations * comps);

    unsafe {
        run.run
            .run_raw(
                &bin,
                invocations.try_into().unwrap(),
                (comps * 4).try_into().unwrap(),
                data.as_mut_ptr() as *mut std::os::raw::c_void,
                data.len() * 4,
            )
            .unwrap();
    }

    // Now, check the results
    for invoc_id in 0..invocations {
        let data = &data[(invoc_id * comps)..((invoc_id + 1) * comps)];

        let mut c = 0_usize;
        for src in &mut fold_src {
            match src {
                FoldData::Pred(b) => {
                    let u = data[c];
                    *b = (u & 1) != 0;
                    c += 1;
                }
                FoldData::U32(u) => {
                    *u = data[c];
                    c += 1;
                }
                FoldData::Vec2(v) => {
                    *v = [data[c + 0], data[c + 1]];
                    c += 2;
                }
            }
        }
        debug_assert!(c == src_comps);

        let mut fold = OpFoldData {
            srcs: &fold_src,
            dsts: &mut fold_dst,
        };
        op.fold(&*run.sm, &mut fold);

        debug_assert!(fold_dst.len() == op.dsts_as_slice().len());
        for (i, dst) in fold_dst.iter().enumerate() {
            match dst {
                FoldData::Pred(b) => {
                    let d = data[c];
                    c += 1;
                    assert_eq!(*b, (d & 1) != 0);
                }
                FoldData::U32(u) => {
                    let d = data[c];
                    c += 1;

                    match dst_types[i] {
                        DstType::GPR => {
                            assert_eq!(*u, d);
                        }
                        DstType::F32 => {
                            assert!(f32_eq(
                                f32::from_bits(*u),
                                f32::from_bits(d)
                            ));
                        }
                        typ => panic!("Can't auto-test {typ:?} data"),
                    }
                }
                FoldData::Vec2(v) => {
                    let d = [data[c + 0], data[c + 1]];
                    c += 2;

                    match dst_types[i] {
                        DstType::F64 => {
                            let v_f64 = f64::from_bits(
                                u64::from(v[0]) | (u64::from(v[1]) << 32),
                            );
                            let d_f64 = f64::from_bits(
                                u64::from(d[0]) | (u64::from(d[1]) << 32),
                            );
                            assert!(f64_eq(v_f64, d_f64));
                        }
                        typ => panic!("Can't auto-test {typ:?} data"),
                    }
                }
            }
        }
        debug_assert!(c == comps);
    }
}

pub fn test_foldable_op(op: impl Foldable + Clone + Into<Op>) {
    let mut a = Acorn::new();
    test_foldable_op_with(op, &mut |_| a.get_u32());
}

#[test]
fn test_op_iabs() {
    if RunSingleton::get().sm.sm() >= 70 {
        let op = OpIAbs {
            dst: Dst::None,
            src: 0.into(),
        };
        test_foldable_op(op);
    }
}

#[test]
fn test_op_iadd3() {
    if RunSingleton::get().sm.sm() >= 70 {
        for i in 0..6 {
            let mut op = OpIAdd3 {
                dst: Dst::None,
                overflow: [Dst::None, Dst::None],
                srcs: [0.into(), 0.into(), 0.into()],
            };
            if i % 3 == 1 {
                op.srcs[0].src_mod = SrcMod::INeg;
            } else if i % 3 == 2 {
                op.srcs[1].src_mod = SrcMod::INeg;
            }
            if i / 3 == 1 {
                op.srcs[2].src_mod = SrcMod::INeg;
            }
            test_foldable_op(op);
        }
    }
}

#[test]
fn test_op_iadd3x() {
    if RunSingleton::get().sm.sm() >= 70 {
        for i in 0..6 {
            let mut op = OpIAdd3X {
                dst: Dst::None,
                overflow: [Dst::None, Dst::None],
                srcs: [0.into(), 0.into(), 0.into()],
                carry: [false.into(), false.into()],
            };
            if i % 3 == 1 {
                op.srcs[0].src_mod = SrcMod::BNot;
            } else if i % 3 == 2 {
                op.srcs[1].src_mod = SrcMod::BNot;
            }
            if i / 3 == 1 {
                op.srcs[2].src_mod = SrcMod::BNot;
            }
            test_foldable_op(op);
        }
    }
}

#[test]
fn test_op_shf() {
    let sm = &RunSingleton::get().sm;

    let types = [IntType::U32, IntType::I32, IntType::U64, IntType::I64];

    for i in 0..32 {
        let op = OpShf {
            dst: Dst::None,
            low: 0.into(),
            high: 0.into(),
            shift: 0.into(),
            data_type: types[i & 0x3],
            right: i & 0x4 != 0,
            wrap: i & 0x8 != 0,
            dst_high: i & 0x10 != 0,
        };

        if sm.sm() < 70 && !(op.dst_high || op.right) {
            continue;
        }

        let shift_idx = op.src_idx(&op.shift);
        let mut a = Acorn::new();
        test_foldable_op_with(op, &mut |i| {
            if i == shift_idx {
                a.get_uint(6) as u32
            } else {
                a.get_u32()
            }
        });
    }
}

#[test]
fn test_op_prmt() {
    let op = OpPrmt {
        dst: Dst::None,
        srcs: [0.into(), 0.into()],
        sel: 0.into(),
        mode: PrmtMode::Index,
    };
    test_foldable_op(op);
}

#[test]
fn test_shl64() {
    let run = RunSingleton::get();
    let invocations = 100;

    let mut b = TestShaderBuilder::new(run.sm.as_ref());

    let srcs = SSARef::from([
        b.ld_test_data(0, MemType::B32)[0],
        b.ld_test_data(4, MemType::B32)[0],
    ]);
    let shift = b.ld_test_data(8, MemType::B32);
    let dst = b.shl64(srcs.into(), shift.into());
    b.st_test_data(12, MemType::B32, dst[0].into());
    b.st_test_data(16, MemType::B32, dst[1].into());

    let bin = b.compile();

    let mut a = Acorn::new();
    let mut data = Vec::new();
    for _ in 0..invocations {
        data.push([a.get_u32(), a.get_u32(), a.get_uint(7) as u32, 0, 0]);
    }

    run.run.run(&bin, &mut data).unwrap();

    for d in &data {
        let src = u64::from(d[0]) | (u64::from(d[1]) << 32);
        let dst = src << (d[2] & 0x3f);
        assert_eq!(d[3], dst as u32);
        assert_eq!(d[4], (dst >> 32) as u32);
    }
}

#[test]
fn test_shr64() {
    let run = RunSingleton::get();
    let invocations = 100;

    let cases = [true, false];

    for signed in cases {
        let mut b = TestShaderBuilder::new(run.sm.as_ref());

        let srcs = SSARef::from([
            b.ld_test_data(0, MemType::B32)[0],
            b.ld_test_data(4, MemType::B32)[0],
        ]);
        let shift = b.ld_test_data(8, MemType::B32);
        let dst = b.shr64(srcs.into(), shift.into(), signed);
        b.st_test_data(12, MemType::B32, dst[0].into());
        b.st_test_data(16, MemType::B32, dst[1].into());

        let bin = b.compile();

        let mut a = Acorn::new();
        let mut data = Vec::new();
        for _ in 0..invocations {
            data.push([a.get_u32(), a.get_u32(), a.get_uint(7) as u32, 0, 0]);
        }

        run.run.run(&bin, &mut data).unwrap();

        for d in &data {
            let src = u64::from(d[0]) | (u64::from(d[1]) << 32);
            let dst = if signed {
                ((src as i64) >> (d[2] & 0x3f)) as u64
            } else {
                src >> (d[2] & 0x3f)
            };
            assert_eq!(d[3], dst as u32);
            assert_eq!(d[4], (dst >> 32) as u32);
        }
    }
}
