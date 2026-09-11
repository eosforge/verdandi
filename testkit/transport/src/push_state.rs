//! 测试状态与统计共用件. 真实字符串键和独立小对象体现注册项基数的成本.
use crate::generated::Domain;
use prost::bytes::Bytes;
use std::{collections::HashMap, io};

#[derive(Clone, Copy)]
pub struct Shape {
    pub registries: usize,
    pub catalogs: usize,
    pub bytes: usize,
}

impl Default for Shape {
    fn default() -> Self {
        Self { registries: 1000, catalogs: 1000, bytes: 256 }
    }
}

impl Shape {
    pub fn validate(self) -> io::Result<()> {
        if !(1..=100000).contains(&self.registries) || !(1..=10000).contains(&self.catalogs) || !(64..=1024).contains(&self.bytes) {
            return Err(io::Error::other("invalid registry/catalog shape"));
        }
        Ok(())
    }

    pub fn key(self, domain: Domain, revision: u64) -> String {
        let (prefix, count) = if domain == Domain::Registry { ("registry/service/instance", self.registries) } else { ("catalog/config", self.catalogs) };
        format!("{prefix}/{:06}", revision % count as u64)
    }

    // 在加入屏障前预置同一份确定性状态. 基准衡量暖缓存增量, 不把预置冒充网络全量同步.
    pub fn state(self) -> HashMap<String, (u64, Bytes)> {
        let mut state = HashMap::with_capacity(self.registries + self.catalogs);
        for (domain, count, size) in [(Domain::Registry, self.registries, 128), (Domain::Catalog, self.catalogs, self.bytes)] {
            for index in 0..count {
                let mut data = vec![0x31; size];
                data[..8].copy_from_slice(&(index as u64).to_le_bytes());
                state.insert(self.key(domain, index as u64), (0, Bytes::from(data)));
            }
        }
        state
    }
}

/// 100 us 桶, 最后一桶明确表示 >= 10 s. 内存不随消息数量增长.
pub struct Histogram(pub Vec<u64>);
impl Default for Histogram {
    fn default() -> Self {
        Self(vec![0; 100001])
    }
}
impl Histogram {
    pub fn record(&mut self, micros: u64) {
        self.0[(micros / 100).min(100000) as usize] += 1;
    }
    pub fn count(&self) -> u64 {
        self.0.iter().sum()
    }
    pub fn percentile(&self, fraction: f64) -> u64 {
        let target = (self.count() as f64 * fraction).ceil() as u64;
        if target == 0 {
            return 0;
        }
        let mut count = 0;
        for (bucket, value) in self.0.iter().enumerate() {
            count += value;
            if count >= target {
                return (bucket as u64 + 1) * 100;
            }
        }
        10000100
    }
}
