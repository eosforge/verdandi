//! 六字节 TCP 对照帧. 与 gRPC 共用 Transfer, 不改变正式 MessageID 表.
use prost::{
    Message,
    bytes::{Bytes, BytesMut},
};
use std::io;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

/// 消息编号只存在于这个独立实验端口, 不连接任何正式 Peer 端口.
pub const HELLO_ID: u16 = 1;
pub const TRANSFER_ID: u16 = 2;

pub struct Reader<R> {
    stream: R,
    buffer: BytesMut,
}

#[cfg(test)]
#[path = "../tests/wire.rs"]
mod tests;

impl<R: AsyncRead + Unpin> Reader<R> {
    pub fn new(stream: R) -> Self {
        Self { stream, buffer: BytesMut::with_capacity(128) }
    }

    pub async fn read(&mut self, id: u16, limit: usize) -> io::Result<Option<Bytes>> {
        // 只有帧边界的 EOF 是正常结束. 部分帧和未知 ID 立即失败.
        let mut header = [0; 6];
        if self.stream.read(&mut header[..1]).await? == 0 {
            return Ok(None);
        }
        self.stream.read_exact(&mut header[1..]).await?;
        let length = u32::from_be_bytes([header[2], header[3], header[4], header[5]]) as usize;
        if u16::from_be_bytes([header[0], header[1]]) != id || length > limit {
            return Err(io::Error::other("invalid probe frame"));
        }
        self.buffer.resize(length, 0);
        self.stream.read_exact(&mut self.buffer).await?;
        Ok(Some(self.buffer.split_to(length).freeze()))
    }
}

pub struct Writer<W> {
    stream: W,
    buffer: Vec<u8>,
}

impl<W: AsyncWrite + Unpin> Writer<W> {
    pub fn new(stream: W) -> Self {
        Self { stream, buffer: Vec::with_capacity(128) }
    }

    pub async fn bytes(&mut self, id: u16, data: &[u8], limit: usize) -> io::Result<()> {
        if data.len() > limit {
            return Err(io::Error::other("probe message limit"));
        }
        self.buffer.clear();
        self.buffer.extend_from_slice(&id.to_be_bytes());
        self.buffer.extend_from_slice(&(data.len() as u32).to_be_bytes());
        self.buffer.extend_from_slice(data);
        self.stream.write_all(&self.buffer).await?;
        self.flush().await
    }

    pub async fn message<M: Message>(&mut self, message: &M, limit: usize) -> io::Result<()> {
        self.message_id(TRANSFER_ID, message, limit).await?;
        self.flush().await
    }

    pub async fn message_id<M: Message>(&mut self, id: u16, message: &M, limit: usize) -> io::Result<()> {
        self.buffer.clear();
        self.append(id, message, limit)?;
        self.stream.write_all(&self.buffer).await
    }

    fn append<M: Message>(&mut self, id: u16, message: &M, limit: usize) -> io::Result<()> {
        let length = message.encoded_len();
        if length > limit {
            return Err(io::Error::other("probe message limit"));
        }
        self.buffer.extend_from_slice(&id.to_be_bytes());
        self.buffer.extend_from_slice(&(length as u32).to_be_bytes());
        message.encode(&mut self.buffer)?;
        Ok(())
    }

    /// 合并已经就绪的完整帧, 一次写入并刷新. 每帧仍有独立六字节头和独立长度校验.
    pub async fn batch<M: Message>(&mut self, id: u16, messages: impl Iterator<Item = io::Result<M>>, limit: usize) -> io::Result<()> {
        self.buffer.clear();
        for (index, message) in messages.enumerate() {
            if index >= 16 {
                return Err(io::Error::other("TCP batch limit"));
            }
            self.append(id, &message?, limit)?;
        }
        self.stream.write_all(&self.buffer).await?;
        self.flush().await
    }

    pub async fn flush(&mut self) -> io::Result<()> {
        self.stream.flush().await
    }

    pub async fn shutdown(&mut self) -> io::Result<()> {
        self.stream.shutdown().await
    }
}
