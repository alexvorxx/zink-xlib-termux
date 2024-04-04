use crate::compiler::nir::*;
use crate::pipe::fence::*;
use crate::pipe::resource::*;
use crate::pipe::screen::*;
use crate::pipe::transfer::*;

use mesa_rust_gen::*;
use mesa_rust_util::has_required_feature;

use std::os::raw::*;
use std::ptr;
use std::ptr::*;
use std::sync::Arc;

pub struct PipeContext {
    pipe: NonNull<pipe_context>,
    screen: Arc<PipeScreen>,
}

unsafe impl Send for PipeContext {}
unsafe impl Sync for PipeContext {}

#[derive(Clone, Copy)]
#[repr(u32)]
pub enum RWFlags {
    RD = pipe_map_flags::PIPE_MAP_READ.0,
    WR = pipe_map_flags::PIPE_MAP_WRITE.0,
    RW = pipe_map_flags::PIPE_MAP_READ_WRITE.0,
}

impl From<RWFlags> for pipe_map_flags {
    fn from(rw: RWFlags) -> Self {
        pipe_map_flags(rw as u32)
    }
}

pub enum ResourceMapType {
    Normal,
    Async,
    Coherent,
}

impl From<ResourceMapType> for pipe_map_flags {
    fn from(map_type: ResourceMapType) -> Self {
        match map_type {
            ResourceMapType::Normal => pipe_map_flags(0),
            ResourceMapType::Async => pipe_map_flags::PIPE_MAP_UNSYNCHRONIZED,
            ResourceMapType::Coherent => {
                pipe_map_flags::PIPE_MAP_COHERENT
                    | pipe_map_flags::PIPE_MAP_PERSISTENT
                    | pipe_map_flags::PIPE_MAP_UNSYNCHRONIZED
            }
        }
    }
}

impl PipeContext {
    pub(super) fn new(context: *mut pipe_context, screen: &Arc<PipeScreen>) -> Option<Self> {
        let s = Self {
            pipe: NonNull::new(context)?,
            screen: screen.clone(),
        };

        if !has_required_cbs(unsafe { s.pipe.as_ref() }) {
            assert!(false, "Context missing features. This should never happen!");
            return None;
        }

        Some(s)
    }

    pub fn buffer_subdata(
        &self,
        res: &PipeResource,
        offset: c_uint,
        data: *const c_void,
        size: c_uint,
    ) {
        unsafe {
            self.pipe.as_ref().buffer_subdata.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                pipe_map_flags::PIPE_MAP_WRITE.0, // TODO PIPE_MAP_x
                offset,
                size,
                data,
            )
        }
    }

    pub fn texture_subdata(
        &self,
        res: &PipeResource,
        bx: &pipe_box,
        data: *const c_void,
        stride: u32,
        layer_stride: u32,
    ) {
        unsafe {
            self.pipe.as_ref().texture_subdata.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                0,
                pipe_map_flags::PIPE_MAP_WRITE.0, // TODO PIPE_MAP_x
                bx,
                data,
                stride,
                layer_stride,
            )
        }
    }

    pub fn clear_buffer(&self, res: &PipeResource, pattern: &[u8], offset: u32, size: u32) {
        unsafe {
            self.pipe.as_ref().clear_buffer.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                offset,
                size,
                pattern.as_ptr().cast(),
                pattern.len() as i32,
            )
        }
    }

    pub fn clear_texture(&self, res: &PipeResource, pattern: &[u32], bx: &pipe_box) {
        unsafe {
            self.pipe.as_ref().clear_texture.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                0,
                bx,
                pattern.as_ptr().cast(),
            )
        }
    }

    pub fn resource_copy_region(
        &self,
        src: &PipeResource,
        dst: &PipeResource,
        dst_offset: &[u32; 3],
        bx: &pipe_box,
    ) {
        unsafe {
            self.pipe.as_ref().resource_copy_region.unwrap()(
                self.pipe.as_ptr(),
                dst.pipe(),
                0,
                dst_offset[0],
                dst_offset[1],
                dst_offset[2],
                src.pipe(),
                0,
                bx,
            )
        }
    }

    fn resource_map(
        &self,
        res: &PipeResource,
        bx: &pipe_box,
        flags: pipe_map_flags,
        is_buffer: bool,
    ) -> Option<PipeTransfer> {
        let mut out: *mut pipe_transfer = ptr::null_mut();

        let ptr = unsafe {
            let func = if is_buffer {
                self.pipe.as_ref().buffer_map
            } else {
                self.pipe.as_ref().texture_map
            };

            func.unwrap()(self.pipe.as_ptr(), res.pipe(), 0, flags.0, bx, &mut out)
        };

        if ptr.is_null() {
            None
        } else {
            Some(PipeTransfer::new(is_buffer, out, ptr))
        }
    }

    fn _buffer_map(
        &self,
        res: &PipeResource,
        offset: i32,
        size: i32,
        flags: pipe_map_flags,
    ) -> Option<PipeTransfer> {
        let b = pipe_box {
            x: offset,
            width: size,
            height: 1,
            depth: 1,
            ..Default::default()
        };

        self.resource_map(res, &b, flags, true)
    }

    pub fn buffer_map(
        &self,
        res: &PipeResource,
        offset: i32,
        size: i32,
        rw: RWFlags,
        map_type: ResourceMapType,
    ) -> PipeTransfer {
        let mut flags: pipe_map_flags = map_type.into();
        flags |= rw.into();
        self._buffer_map(res, offset, size, flags).unwrap()
    }

    pub fn buffer_map_directly(
        &self,
        res: &PipeResource,
        offset: i32,
        size: i32,
        rw: RWFlags,
    ) -> Option<PipeTransfer> {
        let flags =
            pipe_map_flags::PIPE_MAP_DIRECTLY | pipe_map_flags::PIPE_MAP_UNSYNCHRONIZED | rw.into();
        self._buffer_map(res, offset, size, flags)
    }

    pub(super) fn buffer_unmap(&self, tx: *mut pipe_transfer) {
        unsafe { self.pipe.as_ref().buffer_unmap.unwrap()(self.pipe.as_ptr(), tx) };
    }

    pub fn _texture_map(
        &self,
        res: &PipeResource,
        bx: &pipe_box,
        flags: pipe_map_flags,
    ) -> Option<PipeTransfer> {
        self.resource_map(res, bx, flags, false)
    }

    pub fn texture_map(
        &self,
        res: &PipeResource,
        bx: &pipe_box,
        rw: RWFlags,
        map_type: ResourceMapType,
    ) -> PipeTransfer {
        let mut flags: pipe_map_flags = map_type.into();
        flags |= rw.into();
        self._texture_map(res, bx, flags).unwrap()
    }

    pub fn texture_map_directly(
        &self,
        res: &PipeResource,
        bx: &pipe_box,
        rw: RWFlags,
    ) -> Option<PipeTransfer> {
        let flags =
            pipe_map_flags::PIPE_MAP_DIRECTLY | pipe_map_flags::PIPE_MAP_UNSYNCHRONIZED | rw.into();
        self.resource_map(res, bx, flags, false)
    }

    pub(super) fn texture_unmap(&self, tx: *mut pipe_transfer) {
        unsafe { self.pipe.as_ref().texture_unmap.unwrap()(self.pipe.as_ptr(), tx) };
    }

    pub fn create_compute_state(&self, nir: &NirShader, static_local_mem: u32) -> *mut c_void {
        let state = pipe_compute_state {
            ir_type: pipe_shader_ir::PIPE_SHADER_IR_NIR,
            prog: nir.dup_for_driver().cast(),
            req_input_mem: 0,
            static_shared_mem: static_local_mem,
        };
        unsafe { self.pipe.as_ref().create_compute_state.unwrap()(self.pipe.as_ptr(), &state) }
    }

    pub fn bind_compute_state(&self, state: *mut c_void) {
        unsafe { self.pipe.as_ref().bind_compute_state.unwrap()(self.pipe.as_ptr(), state) }
    }

    pub fn delete_compute_state(&self, state: *mut c_void) {
        unsafe { self.pipe.as_ref().delete_compute_state.unwrap()(self.pipe.as_ptr(), state) }
    }

    pub fn create_sampler_state(&self, state: &pipe_sampler_state) -> *mut c_void {
        unsafe { self.pipe.as_ref().create_sampler_state.unwrap()(self.pipe.as_ptr(), state) }
    }

    pub fn bind_sampler_states(&self, samplers: &[*mut c_void]) {
        let mut samplers = samplers.to_owned();
        unsafe {
            self.pipe.as_ref().bind_sampler_states.unwrap()(
                self.pipe.as_ptr(),
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                0,
                samplers.len() as u32,
                samplers.as_mut_ptr(),
            )
        }
    }

    pub fn clear_sampler_states(&self, count: u32) {
        unsafe {
            self.pipe.as_ref().bind_sampler_states.unwrap()(
                self.pipe.as_ptr(),
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                0,
                count,
                ptr::null_mut(),
            )
        }
    }

    pub fn delete_sampler_state(&self, ptr: *mut c_void) {
        unsafe { self.pipe.as_ref().delete_sampler_state.unwrap()(self.pipe.as_ptr(), ptr) }
    }

    pub fn set_constant_buffer(&self, idx: u32, data: &[u8]) {
        let cb = pipe_constant_buffer {
            buffer: ptr::null_mut(),
            buffer_offset: 0,
            buffer_size: data.len() as u32,
            user_buffer: data.as_ptr().cast(),
        };
        unsafe {
            self.pipe.as_ref().set_constant_buffer.unwrap()(
                self.pipe.as_ptr(),
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                idx,
                false,
                &cb,
            )
        }
    }

    pub fn launch_grid(
        &self,
        work_dim: u32,
        block: [u32; 3],
        grid: [u32; 3],
        variable_local_mem: u32,
    ) {
        let info = pipe_grid_info {
            pc: 0,
            input: ptr::null(),
            variable_shared_mem: variable_local_mem,
            work_dim: work_dim,
            block: block,
            last_block: [0; 3],
            grid: grid,
            grid_base: [0; 3],
            indirect: ptr::null_mut(),
            indirect_offset: 0,
        };
        unsafe { self.pipe.as_ref().launch_grid.unwrap()(self.pipe.as_ptr(), &info) }
    }

    pub fn set_global_binding(&self, res: &[Arc<PipeResource>], out: &mut [*mut u32]) {
        let mut res: Vec<_> = res.iter().map(|r| r.pipe()).collect();
        unsafe {
            self.pipe.as_ref().set_global_binding.unwrap()(
                self.pipe.as_ptr(),
                0,
                res.len() as u32,
                res.as_mut_ptr(),
                out.as_mut_ptr(),
            )
        }
    }

    pub fn create_sampler_view(
        &self,
        res: &PipeResource,
        format: pipe_format,
    ) -> *mut pipe_sampler_view {
        let template = res.pipe_sampler_view_template(format);
        unsafe {
            self.pipe.as_ref().create_sampler_view.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                &template,
            )
        }
    }

    pub fn clear_global_binding(&self, count: u32) {
        unsafe {
            self.pipe.as_ref().set_global_binding.unwrap()(
                self.pipe.as_ptr(),
                0,
                count,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        }
    }

    pub fn set_sampler_views(&self, views: &mut [*mut pipe_sampler_view]) {
        unsafe {
            self.pipe.as_ref().set_sampler_views.unwrap()(
                self.pipe.as_ptr(),
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                0,
                views.len() as u32,
                0,
                false,
                views.as_mut_ptr(),
            )
        }
    }

    pub fn clear_sampler_views(&self, count: u32) {
        unsafe {
            self.pipe.as_ref().set_sampler_views.unwrap()(
                self.pipe.as_ptr(),
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                0,
                count,
                0,
                false,
                ptr::null_mut(),
            )
        }
    }

    pub fn sampler_view_destroy(&self, view: *mut pipe_sampler_view) {
        unsafe { self.pipe.as_ref().sampler_view_destroy.unwrap()(self.pipe.as_ptr(), view) }
    }

    pub fn set_shader_images(&self, images: &[pipe_image_view]) {
        unsafe {
            self.pipe.as_ref().set_shader_images.unwrap()(
                self.pipe.as_ptr(),
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                0,
                images.len() as u32,
                0,
                images.as_ptr(),
            )
        }
    }

    pub fn clear_shader_images(&self, count: u32) {
        unsafe {
            self.pipe.as_ref().set_shader_images.unwrap()(
                self.pipe.as_ptr(),
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                0,
                count,
                0,
                ptr::null_mut(),
            )
        }
    }

    pub fn memory_barrier(&self, barriers: u32) {
        unsafe { self.pipe.as_ref().memory_barrier.unwrap()(self.pipe.as_ptr(), barriers) }
    }

    pub fn flush(&self) -> PipeFence {
        unsafe {
            let mut fence = ptr::null_mut();
            self.pipe.as_ref().flush.unwrap()(self.pipe.as_ptr(), &mut fence, 0);
            PipeFence::new(fence, &self.screen)
        }
    }
}

impl Drop for PipeContext {
    fn drop(&mut self) {
        unsafe {
            self.pipe.as_ref().destroy.unwrap()(self.pipe.as_ptr());
        }
    }
}

fn has_required_cbs(context: &pipe_context) -> bool {
    // Use '&' to evaluate all features and to not stop
    // on first missing one to list all missing features.
    has_required_feature!(context, destroy)
        & has_required_feature!(context, bind_compute_state)
        & has_required_feature!(context, bind_sampler_states)
        & has_required_feature!(context, buffer_map)
        & has_required_feature!(context, buffer_subdata)
        & has_required_feature!(context, buffer_unmap)
        & has_required_feature!(context, clear_buffer)
        & has_required_feature!(context, clear_texture)
        & has_required_feature!(context, create_compute_state)
        & has_required_feature!(context, delete_compute_state)
        & has_required_feature!(context, delete_sampler_state)
        & has_required_feature!(context, flush)
        & has_required_feature!(context, launch_grid)
        & has_required_feature!(context, memory_barrier)
        & has_required_feature!(context, resource_copy_region)
        & has_required_feature!(context, sampler_view_destroy)
        & has_required_feature!(context, set_constant_buffer)
        & has_required_feature!(context, set_global_binding)
        & has_required_feature!(context, set_sampler_views)
        & has_required_feature!(context, set_shader_images)
        & has_required_feature!(context, texture_map)
        & has_required_feature!(context, texture_subdata)
        & has_required_feature!(context, texture_unmap)
}
