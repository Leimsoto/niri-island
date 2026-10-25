//! Models scripts build (`createModelData`), drawn into the scene with a depth
//! buffer: the scene's colour-only pass ends, a pass with the colour loaded
//! and depth (cleared once per run of models) draws them, and the scene goes
//! on in its append pass. Depth follows Wallpaper Engine: reversed, the
//! larger value wins.

use anyhow::Result;
use ash::vk;

use super::scene::{ParticleDraw, SceneTarget};
use super::Renderer;

pub(super) const DEPTH_FORMAT: vk::Format = vk::Format::D32_SFLOAT;

/// A scene target's depth buffer, and the framebuffer joining the two.
pub(super) struct MeshDepth {
    pub color_view: vk::ImageView,
    pub image: vk::Image,
    pub memory: vk::DeviceMemory,
    pub view: vk::ImageView,
    pub framebuffer: vk::Framebuffer,
}

fn create_mesh_pass(device: &ash::Device, format: vk::Format, clear_depth: bool) -> Result<vk::RenderPass> {
    unsafe {
        let attachments = [
            vk::AttachmentDescription::default()
                .format(format)
                .samples(vk::SampleCountFlags::TYPE_1)
                .load_op(vk::AttachmentLoadOp::LOAD)
                .store_op(vk::AttachmentStoreOp::STORE)
                .initial_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                .final_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL),
            vk::AttachmentDescription::default()
                .format(DEPTH_FORMAT)
                .samples(vk::SampleCountFlags::TYPE_1)
                .load_op(if clear_depth { vk::AttachmentLoadOp::CLEAR } else { vk::AttachmentLoadOp::LOAD })
                .store_op(vk::AttachmentStoreOp::STORE)
                .stencil_load_op(vk::AttachmentLoadOp::DONT_CARE)
                .stencil_store_op(vk::AttachmentStoreOp::DONT_CARE)
                .initial_layout(if clear_depth {
                    vk::ImageLayout::UNDEFINED
                } else {
                    vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                })
                .final_layout(vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
        ];
        let color_ref = [vk::AttachmentReference::default()
            .attachment(0)
            .layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)];
        let depth_ref = vk::AttachmentReference::default()
            .attachment(1)
            .layout(vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        let subpass = [vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(&color_ref)
            .depth_stencil_attachment(&depth_ref)];
        let stages_in = vk::PipelineStageFlags::FRAGMENT_SHADER
            | vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT
            | vk::PipelineStageFlags::EARLY_FRAGMENT_TESTS
            | vk::PipelineStageFlags::LATE_FRAGMENT_TESTS
            | vk::PipelineStageFlags::TRANSFER;
        let dependencies = [
            vk::SubpassDependency::default()
                .src_subpass(vk::SUBPASS_EXTERNAL)
                .dst_subpass(0)
                .src_stage_mask(stages_in)
                .dst_stage_mask(
                    vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT
                        | vk::PipelineStageFlags::EARLY_FRAGMENT_TESTS
                        | vk::PipelineStageFlags::LATE_FRAGMENT_TESTS,
                )
                .src_access_mask(
                    vk::AccessFlags::SHADER_READ
                        | vk::AccessFlags::COLOR_ATTACHMENT_WRITE
                        | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE
                        | vk::AccessFlags::TRANSFER_READ,
                )
                .dst_access_mask(
                    vk::AccessFlags::COLOR_ATTACHMENT_READ
                        | vk::AccessFlags::COLOR_ATTACHMENT_WRITE
                        | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ
                        | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE,
                ),
            vk::SubpassDependency::default()
                .src_subpass(0)
                .dst_subpass(vk::SUBPASS_EXTERNAL)
                .src_stage_mask(
                    vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT | vk::PipelineStageFlags::LATE_FRAGMENT_TESTS,
                )
                .dst_stage_mask(
                    vk::PipelineStageFlags::FRAGMENT_SHADER
                        | vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT
                        | vk::PipelineStageFlags::EARLY_FRAGMENT_TESTS,
                )
                .src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE)
                .dst_access_mask(
                    vk::AccessFlags::SHADER_READ
                        | vk::AccessFlags::COLOR_ATTACHMENT_READ
                        | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ,
                ),
        ];
        Ok(device.create_render_pass(
            &vk::RenderPassCreateInfo::default()
                .attachments(&attachments)
                .subpasses(&subpass)
                .dependencies(&dependencies),
            None,
        )?)
    }
}

impl Renderer {
    /// The pass models draw in, for targets of `format` (both variants are
    /// compatible: pipelines made with one work in the other).
    pub fn mesh_pass_for(&mut self, format: vk::Format, clear_depth: bool) -> Result<vk::RenderPass> {
        if let Some((_, _, pass)) = self.mesh_passes.iter().find(|(f, c, _)| *f == format && *c == clear_depth) {
            return Ok(*pass);
        }
        let pass = create_mesh_pass(&self.device, format, clear_depth)?;
        self.mesh_passes.push((format, clear_depth, pass));
        Ok(pass)
    }

    fn mesh_framebuffer(&mut self, target: &SceneTarget) -> Result<vk::Framebuffer> {
        if let Some(depth) = self.mesh_depths.iter().find(|d| d.color_view == target.view) {
            return Ok(depth.framebuffer);
        }
        let pass = self.mesh_pass_for(target.format, true)?;
        let extent = target.extent;
        unsafe {
            let image = self.device.create_image(
                &vk::ImageCreateInfo::default()
                    .image_type(vk::ImageType::TYPE_2D)
                    .format(DEPTH_FORMAT)
                    .extent(vk::Extent3D { width: extent.width, height: extent.height, depth: 1 })
                    .mip_levels(1)
                    .array_layers(1)
                    .samples(vk::SampleCountFlags::TYPE_1)
                    .tiling(vk::ImageTiling::OPTIMAL)
                    .usage(vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT)
                    .sharing_mode(vk::SharingMode::EXCLUSIVE)
                    .initial_layout(vk::ImageLayout::UNDEFINED),
                None,
            )?;
            let reqs = self.device.get_image_memory_requirements(image);
            let memory = self.device.allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(reqs.size)
                    .memory_type_index(self.device_local_index(reqs)?),
                None,
            )?;
            self.device.bind_image_memory(image, memory, 0)?;
            let view = self.device.create_image_view(
                &vk::ImageViewCreateInfo::default()
                    .image(image)
                    .view_type(vk::ImageViewType::TYPE_2D)
                    .format(DEPTH_FORMAT)
                    .subresource_range(vk::ImageSubresourceRange {
                        aspect_mask: vk::ImageAspectFlags::DEPTH,
                        base_mip_level: 0,
                        level_count: 1,
                        base_array_layer: 0,
                        layer_count: 1,
                    }),
                None,
            )?;
            let attachments = [target.view, view];
            let framebuffer = self.device.create_framebuffer(
                &vk::FramebufferCreateInfo::default()
                    .render_pass(pass)
                    .attachments(&attachments)
                    .width(extent.width)
                    .height(extent.height)
                    .layers(1),
                None,
            )?;
            self.mesh_depths.push(MeshDepth { color_view: target.view, image, memory, view, framebuffer });
            Ok(framebuffer)
        }
    }

    /// A run of models into `target`, between two parts of its scene pass:
    /// the caller has ended its pass before and resumes after.
    pub(super) fn record_meshes(&mut self, target: &SceneTarget, batches: &[&ParticleDraw<'_>]) -> Result<()> {
        let (w, h) = (target.extent.width, target.extent.height);
        self.profile_mark(|| format!("scene layers at {w}x{h}"));
        let framebuffer = self.mesh_framebuffer(target)?;
        let pass = self.mesh_pass_for(target.format, true)?;
        let extent = target.extent;
        unsafe {
            let clears = [
                vk::ClearValue::default(),
                vk::ClearValue { depth_stencil: vk::ClearDepthStencilValue { depth: 0.0, stencil: 0 } },
            ];
            self.device.cmd_begin_render_pass(
                self.cmd,
                &vk::RenderPassBeginInfo::default()
                    .render_pass(pass)
                    .framebuffer(framebuffer)
                    .render_area(vk::Rect2D { offset: vk::Offset2D { x: 0, y: 0 }, extent })
                    .clear_values(&clears),
                vk::SubpassContents::INLINE,
            );
            self.device.cmd_set_scissor(self.cmd, 0, &[vk::Rect2D { offset: vk::Offset2D { x: 0, y: 0 }, extent }]);
            for batch in batches {
                let Some(zero) = batch.mesh else { continue };
                // Models' shaders place vertices themselves, the Direct3D way
                // (y up): always the flipped viewport.
                let height = extent.height as f32;
                self.device.cmd_set_viewport(
                    self.cmd,
                    0,
                    &[vk::Viewport { x: 0.0, y: height, width: extent.width as f32, height: -height, min_depth: 0.0, max_depth: 1.0 }],
                );
                self.device.cmd_bind_pipeline(self.cmd, vk::PipelineBindPoint::GRAPHICS, batch.pipeline.pipeline);
                self.device.cmd_bind_descriptor_sets(
                    self.cmd,
                    vk::PipelineBindPoint::GRAPHICS,
                    batch.pipeline.layout,
                    0,
                    &[batch.pipeline.set],
                    &[],
                );
                self.device.cmd_bind_vertex_buffers(self.cmd, 0, &[batch.vertices, zero], &[0, 0]);
                self.device.cmd_bind_index_buffer(self.cmd, batch.indices, 0, vk::IndexType::UINT32);
                self.device.cmd_draw_indexed(self.cmd, batch.index_count, 1, 0, 0, 0);
                if self.profile.is_some() {
                    let (label, triangles) = (batch.label.to_owned(), batch.index_count / 3);
                    self.profile_mark(|| format!("  model {label} ({triangles} triangles)"));
                }
            }
            self.device.cmd_end_render_pass(self.cmd);
        }
        let draws = batches.iter().filter(|b| b.mesh.is_some()).count();
        let indices: u32 = batches.iter().filter(|b| b.mesh.is_some()).map(|b| b.index_count).sum();
        self.profile_mark(|| format!("models: {draws} draws, {} triangles at {w}x{h}", indices / 3));
        Ok(())
    }

    pub(super) fn destroy_mesh_resources(&mut self) {
        unsafe {
            for depth in self.mesh_depths.drain(..) {
                self.device.destroy_framebuffer(depth.framebuffer, None);
                self.device.destroy_image_view(depth.view, None);
                self.device.destroy_image(depth.image, None);
                self.device.free_memory(depth.memory, None);
            }
            for (_, _, pass) in self.mesh_passes.drain(..) {
                self.device.destroy_render_pass(pass, None);
            }
        }
    }
}
