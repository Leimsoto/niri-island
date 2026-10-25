//! SKWD_VK_PROFILE: where a scene frame's GPU time goes. Timestamps are
//! written between the parts of the frame (effect chains, snapshots, the
//! final pass) and, every 120 frames, the average time of each part is
//! logged, largest first.

use std::collections::HashMap;

use anyhow::Result;
use ash::vk;

const MAX_MARKS: u32 = 256;
const REPORT_EVERY: u32 = 120;

pub struct GpuProfile {
    pub(super) pool: vk::QueryPool,
    labels: Vec<String>,
    totals: HashMap<String, (f64, u32)>,
    frames: u32,
}

impl GpuProfile {
    pub fn new(device: &ash::Device) -> Result<Self> {
        let pool = unsafe {
            device.create_query_pool(
                &vk::QueryPoolCreateInfo::default().query_type(vk::QueryType::TIMESTAMP).query_count(MAX_MARKS),
                None,
            )?
        };
        Ok(Self { pool, labels: Vec::new(), totals: HashMap::new(), frames: 0 })
    }

    pub fn begin(&mut self, device: &ash::Device, cmd: vk::CommandBuffer) {
        self.labels.clear();
        unsafe {
            device.cmd_reset_query_pool(cmd, self.pool, 0, MAX_MARKS);
            device.cmd_write_timestamp(cmd, vk::PipelineStageFlags::TOP_OF_PIPE, self.pool, 0);
        }
        self.labels.push(String::new());
    }

    pub fn mark(&mut self, device: &ash::Device, cmd: vk::CommandBuffer, label: String) {
        let index = self.labels.len() as u32;
        if index >= MAX_MARKS || self.labels.is_empty() {
            return;
        }
        unsafe { device.cmd_write_timestamp(cmd, vk::PipelineStageFlags::BOTTOM_OF_PIPE, self.pool, index) };
        self.labels.push(label);
    }

    /// After the frame's fence: reads the marks and, now and then, reports.
    pub fn collect(&mut self, device: &ash::Device, period: f64, valid_bits: u32) {
        let count = self.labels.len();
        if count < 2 {
            return;
        }
        let mut values = vec![0u64; count];
        let read = unsafe {
            device.get_query_pool_results(self.pool, 0, &mut values, vk::QueryResultFlags::TYPE_64)
        };
        if read.is_err() {
            return;
        }
        let mask = if valid_bits >= 64 { u64::MAX } else { (1u64 << valid_bits) - 1 };
        for i in 1..count {
            let ticks = values[i].wrapping_sub(values[i - 1]) & mask;
            let entry = self.totals.entry(self.labels[i].clone()).or_insert((0.0, 0));
            entry.0 += ticks as f64 * period / 1e6;
            entry.1 += 1;
        }
        self.frames += 1;
        if self.frames < REPORT_EVERY {
            return;
        }
        let frames = f64::from(self.frames);
        let mut rows: Vec<(String, f64)> =
            self.totals.drain().map(|(label, (ms, _))| (label, ms / frames)).collect();
        rows.sort_by(|a, b| b.1.total_cmp(&a.1));
        let total: f64 = rows.iter().map(|(_, ms)| ms).sum();
        tracing::info!("skwd-wall-vk: profile {total:.2} ms/frame over {} frames", self.frames);
        for (label, ms) in rows.iter().take(24) {
            tracing::info!("skwd-wall-vk: profile {ms:7.2} ms  {label}");
        }
        self.frames = 0;
    }
}
