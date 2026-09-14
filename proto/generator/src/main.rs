//! 显式生成 Go 协议源码和稳定 MessageID, 不下载工具或依赖.
//!
//! 所有结果先进入本次调用独占的临时目录. --check 仅比较, 默认模式只在全部生成成功后更新源码.

use std::{
    collections::BTreeMap,
    env,
    error::Error,
    fs, io,
    path::{Path, PathBuf},
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};

use prost::Message;

mod ids;

/// 临时目录只包含生成器创建的普通文件, Drop 逐个清理, 不递归删除任何目录.
struct Scratch(PathBuf);

impl Scratch {
    fn create(root: &Path) -> io::Result<Self> {
        let parent = root.join("build/proto");
        fs::create_dir_all(&parent)?;
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH).map_err(io::Error::other)?.as_nanos();
        let path = parent.join(format!("generate-{}-{nonce}", std::process::id()));
        // create_dir 而不是 create_dir_all, 已存在即失败, 避免清理其他调用拥有的输出.
        fs::create_dir(&path)?;
        Ok(Self(path))
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        if let Ok(entries) = fs::read_dir(&self.0) {
            for entry in entries.flatten() {
                if entry.file_type().is_ok_and(|kind| kind.is_file()) {
                    let _ = fs::remove_file(entry.path());
                }
            }
        }
        let _ = fs::remove_dir(&self.0);
    }
}

/// 解析唯一的检查开关, 从自身 manifest 定位仓库, 不依赖调用目录或环境开关.
fn main() -> Result<(), Box<dyn Error>> {
    let arguments: Vec<_> = env::args_os().skip(1).collect();
    let check = match arguments.as_slice() {
        [] => false,
        [argument] if argument == "--check" => true,
        _ => return Err(io::Error::new(io::ErrorKind::InvalidInput, "usage: generate-proto [--check]").into()),
    };
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .ok_or_else(|| io::Error::other("missing repository root"))?;
    let scratch = Scratch::create(root)?;
    let proto_root = root.join("proto");
    let mut schemas: Vec<_> = fs::read_dir(&proto_root)?
        .collect::<io::Result<Vec<_>>>()?
        .into_iter()
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|extension| extension == "proto"))
        .collect();
    schemas.sort();
    if schemas.is_empty() {
        return Err(io::Error::other("no protocol schemas").into());
    }

    // 使用项目内已准备的固定版本. 显式路径只影响当前生成命令, 不写用户配置.
    let executable = |name: &str| if cfg!(windows) { format!("{name}.exe") } else { name.to_owned() };
    let protoc = env::var_os("PROTOC")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("build/tools/protoc/36.1/bin").join(executable("protoc")));
    let go_plugin = root.join("build/tools/protoc-gen-go/1.36.12").join(executable("protoc-gen-go"));
    let grpc_plugin = root.join("build/tools/protoc-gen-go-grpc/1.6.2").join(executable("protoc-gen-go-grpc"));
    require_version(&protoc, "libprotoc 36.1")?;
    require_version(&go_plugin, &format!("{} v1.36.12", executable("protoc-gen-go")))?;
    require_version(&grpc_plugin, "protoc-gen-go-grpc 1.6.2")?;

    // 描述符用于稳定消息编号. 旧 Rust 服务已废弃, 不再生成其运行时代码.
    run(Command::new(&protoc)
        .arg(format!("--descriptor_set_out={}", scratch.0.join("descriptor.pb").display()))
        .arg("--include_imports")
        .arg("-I")
        .arg(&proto_root)
        .args(&schemas))?;
    let mut go = Command::new(&protoc);
    go.arg(format!("--plugin=protoc-gen-go={}", go_plugin.display()))
        .arg(format!("--plugin=protoc-gen-go-grpc={}", grpc_plugin.display()))
        .arg(format!("--go_out={}", scratch.0.display()))
        .arg("--go_opt=paths=source_relative")
        .arg(format!("--go-grpc_out={}", scratch.0.display()))
        .arg("--go-grpc_opt=paths=source_relative")
        .arg("-I")
        .arg(&proto_root)
        .args(&schemas);
    run(&mut go)?;
    let registry = generate_ids(&scratch.0, &proto_root)?;
    run(Command::new("gofmt").arg("-w").arg(scratch.0.join("message_ids.go")))?;

    // 先收集和比较全部文件, --check 永远不写入源码. descriptor 仅为本次编号分配的中间输入.
    let mut outputs = BTreeMap::new();
    outputs.insert(proto_root.join("message-ids.lock"), registry.into_bytes());
    for entry in fs::read_dir(&scratch.0)? {
        let path = entry?.path();
        let destination = match path.extension().and_then(|extension| extension.to_str()) {
            Some("go") => root.join("supervisor/internal/generated"),
            _ => continue,
        };
        let name = path.file_name().ok_or_else(|| io::Error::other("missing generated file name"))?;
        outputs.insert(destination.join(name), fs::read(&path)?);
    }
    // 生成目录只保存本工具拥有的源文件. 多余文件必须显式移除, 避免删除 schema 后仍编译旧消息.
    for directory in [root.join("supervisor/internal/generated")] {
        if !directory.exists() {
            continue;
        }
        for entry in fs::read_dir(&directory)? {
            let path = entry?.path();
            if !outputs.contains_key(&path) {
                return Err(io::Error::other(format!("unexpected generated file: {}", path.display())).into());
            }
        }
    }
    let stale: Vec<_> = outputs
        .iter()
        .filter(|(path, bytes)| fs::read(path).as_deref().ok() != Some(bytes.as_slice()))
        .collect();
    if check && !stale.is_empty() {
        for (path, _) in &stale {
            eprintln!("stale: {}", path.display());
        }
        return Err(io::Error::other("generated protocol differs; run scripts/generate-proto").into());
    }
    for (path, bytes) in stale {
        fs::create_dir_all(path.parent().ok_or_else(|| io::Error::other("missing output parent"))?)?;
        if path == &proto_root.join("message-ids.lock") {
            ids::store(path, &fs::read_to_string(path)?, std::str::from_utf8(bytes)?, true)?;
        } else {
            fs::write(path, bytes)?;
        }
        println!("generated: {}", path.display());
    }
    println!("protocol {}", if check { "is current" } else { "generated" });
    Ok(())
}

/// 工具失败保留退出状态, 不执行下载补救或静默改用另一个版本.
fn run(command: &mut Command) -> io::Result<()> {
    let status = command.status()?;
    if !status.success() {
        return Err(io::Error::other(format!("protocol tool exited with {status}")));
    }
    Ok(())
}

/// 生成器与运行库固定版本配套, 避免两端输出因本机 PATH 不同而漂移.
fn require_version(tool: &Path, expected: &str) -> io::Result<()> {
    let output = Command::new(tool)
        .arg("--version")
        .output()
        .map_err(|error| io::Error::new(error.kind(), format!("{}: {error}; see proto/README.md", tool.display())))?;
    if !output.status.success() || String::from_utf8_lossy(&output.stdout).trim() != expected {
        return Err(io::Error::other(format!("expected {expected}")));
    }
    Ok(())
}

/// 为全部顶层消息追加稳定编号, 嵌套结构不分配 MessageID. 两种语言共用一份映射.
fn generate_ids(output: &Path, proto_root: &Path) -> Result<String, Box<dyn Error>> {
    let descriptor = prost_types::FileDescriptorSet::decode(fs::read(output.join("descriptor.pb"))?.as_slice())?;
    let mut messages = BTreeMap::new();
    for file in descriptor
        .file
        .iter()
        .filter(|file| file.name.as_ref().is_some_and(|name| !name.starts_with("google/")))
    {
        let package = file.package.as_deref().ok_or_else(|| io::Error::other("protocol package is required"))?;
        for message in &file.message_type {
            let name = message.name.as_deref().ok_or_else(|| io::Error::other("unnamed message"))?;
            if package != "verdandi.cluster.v1" {
                return Err(io::Error::other("unsupported control protocol package").into());
            }
            messages.insert(format!("{package}.{name}"), name.to_owned());
        }
    }
    let previous = fs::read_to_string(proto_root.join("message-ids.lock"))?;
    let ids = ids::assign(&previous, messages.keys().cloned())?;
    let mut registry = String::from("# Automatically assigned MessageIDs. Commit this file with cluster.proto. Never reuse an ID.\n");
    for (id, name) in &ids {
        registry.push_str(&format!("{id}\t{name}\n"));
    }
    let active: Vec<_> = ids
        .iter()
        .filter_map(|(id, full_name)| messages.get(full_name).map(|name| (*id, name)))
        .collect();

    let mut go = String::from(
        "// Code generated by verdandi-protocol-generator from proto/*.proto and message-ids.lock. DO NOT EDIT.\npackage wire\nimport \"google.golang.org/protobuf/proto\"\ntype MessageID uint16\nconst (\n",
    );
    for (id, name) in &active {
        go.push_str(&format!("ID{name} MessageID = {id}\n"));
    }
    go.push_str(")\nfunc NewMessage(id MessageID) proto.Message { switch id {\n");
    for (_, name) in &active {
        go.push_str(&format!("case ID{name}: return &{name}{{}}\n"));
    }
    go.push_str("default: return nil } }\nfunc IDOf(message proto.Message) (MessageID, bool) { switch message.(type) {\n");
    for (_, name) in &active {
        go.push_str(&format!("case *{name}: return ID{name}, true\n"));
    }
    go.push_str("default: return 0, false } }\n");
    fs::write(output.join("message_ids.go"), go)?;
    Ok(registry)
}
