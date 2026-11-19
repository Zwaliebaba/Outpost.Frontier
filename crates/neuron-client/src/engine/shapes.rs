//! The texture shapes' library calls ([`ShapeTextures`]) on the render
//! stack's texture library and the data file system: what `ShapeDB` and
//! the cursors of `mouse.ini` ask of `ITextureLibrary2` and of the client's
//! `DAEngine_open_filesystem`.

use neuron_core::vfs::{FsDesc, Host, UtfFileSystem};

use crate::render::ff::device::Backend;
use crate::render::stack::RenderStack;
use crate::ui::shape::ShapeTextures;

/// [`ShapeTextures`] over a render stack's texture library, opening the
/// shape files' texture archives under the data.
pub struct StackShapes<'a, B: Backend + 'static> {
    stack: &'a mut RenderStack<B>,
    data: &'a Host,
}

impl<'a, B: Backend + 'static> StackShapes<'a, B> {
    /// The calls on `stack`, archives opened under `data`.
    pub fn new(stack: &'a mut RenderStack<B>, data: &'a Host) -> StackShapes<'a, B> {
        StackShapes { stack, data }
    }
}

impl<B: Backend + 'static> ShapeTextures for StackShapes<'_, B> {
    fn find(&mut self, name: &[u8]) -> Option<u32> {
        // `FindTextureByCrc` and the `Release` that follows it leave the
        // reference count as it was: the name map's lookup.
        self.stack
            .shading
            .textures
            .find(name)
            .and_then(|r| u32::try_from(r).ok())
    }

    fn add_ref(&mut self, record: u32) {
        let _ = self
            .stack
            .shading
            .textures
            .create_playback(Some(record as usize), false);
    }

    fn release(&mut self, record: u32) {
        let (textures, mut pipeline) = self.stack.shading.texture_parts();
        textures.release_record(Some(record as usize), &mut pipeline);
    }

    fn load_group(&mut self, file: &[u8]) -> Option<i32> {
        let mut fs =
            UtfFileSystem::open(self.data, &FsDesc::read(file).with_implementation(b"UTF")).ok()?;
        let (textures, mut pipeline) = self.stack.shading.texture_parts();
        textures.load_group(&mut fs, &mut pipeline, true).group
    }

    fn garbage_collect(&mut self, group: i32) {
        let (textures, mut pipeline) = self.stack.shading.texture_parts();
        textures.garbage_collect(group, &mut pipeline);
    }

    fn generic_slot_4(&mut self) -> u32 {
        self.stack.shading.textures.generic_slot(4).unwrap_or(0)
    }

    fn set_generic_slot_4(&mut self, value: u32) {
        self.stack.shading.textures.set_generic_slot(4, value);
    }

    fn frame_handle(&mut self, record: u32, frame: i32) -> u32 {
        self.stack
            .shading
            .textures
            .texture_frame(Some(record as usize), frame)
            .1
            .handle
    }

    fn texture_dim(&mut self, handle: u32) -> (u32, u32) {
        let shading = &self.stack.shading;
        shading
            .handles
            .get(handle)
            .and_then(|h| shading.device.textures.get(h).ok())
            .map_or((0, 0), |t| (t.width, t.height))
    }
}
