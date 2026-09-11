//! 复用标准 Prost 编码, 在成员列表分配对象前增加数量预算.
use crate::protocol::RegistrationResponse;
use prost::{
    Message,
    bytes::Buf,
    encoding::{DecodeContext, WireType, decode_key, skip_field},
};
use std::{any::TypeId, marker::PhantomData};
use tonic::{
    Status,
    codec::{Codec, DecodeBuf, Decoder},
};

/// 生成的 RPC 共用此 codec, 普通消息保持标准 Prost 路径.
pub struct LimitedCodec<T, U>(PhantomData<(T, U)>);
impl<T, U> Default for LimitedCodec<T, U> {
    fn default() -> Self {
        Self(PhantomData)
    }
}
impl<T, U> Codec for LimitedCodec<T, U>
where
    T: Message + Send + 'static,
    U: Message + Default + Send + 'static,
{
    type Encode = T;
    type Decode = U;
    type Encoder = <tonic_prost::ProstCodec<T, U> as Codec>::Encoder;
    type Decoder = LimitedDecoder<U>;
    fn encoder(&mut self) -> Self::Encoder {
        tonic_prost::ProstCodec::<T, U>::default().encoder()
    }
    fn decoder(&mut self) -> Self::Decoder {
        LimitedDecoder(PhantomData)
    }
}

/// 小型控制消息无需扫描, 只有登记响应含不可信 repeated 对象数量.
pub struct LimitedDecoder<U>(PhantomData<U>);
impl<U: Message + Default + 'static> Decoder for LimitedDecoder<U> {
    type Item = U;
    type Error = Status;
    fn decode(&mut self, buf: &mut DecodeBuf<'_>) -> Result<Option<U>, Status> {
        let bytes = buf.copy_to_bytes(buf.remaining());
        if TypeId::of::<U>() == TypeId::of::<RegistrationResponse>() {
            check_members(bytes.clone(), 4096)?;
        }
        U::decode(bytes).map(Some).map_err(|_| Status::internal("invalid protobuf"))
    }
}

// 两 MiB 内可以塞入数十万个空 Member. 无分配字段扫描先限制对象数量, 再进行正常解码.
fn check_members(mut fields: impl Buf, maximum: usize) -> Result<(), Status> {
    let mut count = 0;
    while fields.has_remaining() {
        let (tag, wire) = decode_key(&mut fields).map_err(|_| Status::internal("invalid protobuf field"))?;
        if tag == 1 {
            if wire != WireType::LengthDelimited {
                return Err(Status::internal("invalid member field"));
            }
            count += 1;
            if count > maximum {
                return Err(Status::resource_exhausted("too many members"));
            }
        }
        skip_field(wire, tag, &mut fields, DecodeContext::default()).map_err(|_| Status::internal("invalid protobuf field"))?;
    }
    Ok(())
}

#[cfg(test)]
#[path = "../tests/unit/codec.rs"]
mod tests;
