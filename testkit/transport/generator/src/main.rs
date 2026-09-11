//! 显式生成独立实验接口, 普通编译不调用 protoc.
//! 显式生成原型 RPC 源码. 普通构建不调用 protoc, 也不写入源码目录.
use std::{env, error::Error, path::PathBuf};

fn main() -> Result<(), Box<dyn Error>> {
    let protoc = env::args_os().nth(1).ok_or("usage: generator <existing-protoc-path>")?;
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let mut prost = prost_build::Config::new();
    // Bytes 保留共享缓冲的能力. 这不意味着 TLS/HTTP2 全路径没有复制.
    prost.protoc_executable(protoc).bytes(["."]);
    tonic_prost_build::configure().out_dir(root.join("src/generated")).compile_with_config(prost, &[root.join("proto/probe.proto")], &[root.join("proto")])?;
    Ok(())
}
