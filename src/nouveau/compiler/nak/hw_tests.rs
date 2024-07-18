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
