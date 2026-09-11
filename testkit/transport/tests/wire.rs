//! 批次必须保留独立消息边界, 并在流转为空闲之前刷新尾部.
use super::*;
use crate::generated::Transfer;
use std::time::Duration;

#[tokio::test]
async fn batch_preserves_frames_and_flushes_without_a_following_write() -> io::Result<()> {
    let (send, receive) = tokio::io::duplex(1024);
    let mut writer = Writer::new(tokio::io::BufWriter::new(send));
    let messages = [Transfer { sequence: 1, body: Bytes::from_static(b"first") }, Transfer { sequence: 2, body: Bytes::from_static(b"last") }];
    writer.batch(TRANSFER_ID, messages.iter().cloned().map(Ok), 128).await?;
    let mut reader = Reader::new(receive);
    for expected in messages {
        let bytes = tokio::time::timeout(Duration::from_millis(100), reader.read(TRANSFER_ID, 128)).await??.ok_or_else(|| io::Error::other("missing frame"))?;
        assert_eq!(Transfer::decode(bytes)?, expected);
    }
    Ok(())
}

#[tokio::test]
async fn oversized_batch_does_not_emit_a_partial_batch() {
    let mut output = Vec::new();
    let mut writer = Writer::new(&mut output);
    let messages = (1..=17).map(|sequence| Ok(Transfer { sequence, body: Bytes::new() }));
    assert!(writer.batch(TRANSFER_ID, messages, 128).await.is_err());
    drop(writer);
    assert!(output.is_empty());
}
