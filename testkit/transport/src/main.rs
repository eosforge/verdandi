//! Star/Planet 传输原型入口. 只允许显式测试地址和仓库公开测试身份.
mod auth;
mod measure;
mod push;
mod push_io;
mod push_measure;
mod push_state;
mod transport;
mod wire;
mod generated {
    include!("generated/verdandi.probe.rs");
}
use generated::Transfer;
use std::{collections::BTreeMap, io, path::PathBuf, sync::Arc, time::Duration};
use verdandi_peer_common::protocol::NodeRole;

#[tokio::main(worker_threads = 2)]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = std::env::args().skip(1);
    let mode = arguments.next().ok_or("expected serve or run")?;
    let mut options = BTreeMap::new();
    for argument in arguments {
        let (name, value) = argument.split_once('=').ok_or("expected --name=value")?;
        if options.insert(name.to_owned(), value.to_owned()).is_some() {
            return Err("duplicate option".into());
        }
    }
    let grpc = match take(&mut options, "--transport", "tcp").as_str() {
        "tcp" => false,
        "grpc" => true,
        _ => return Err("invalid transport".into()),
    };
    let address = take(&mut options, "--address", "127.0.0.1:0").parse()?;
    let fixtures = PathBuf::from(take(&mut options, "--fixtures", "peer/tests/fixtures"));
    let role = match take(&mut options, "--role", "star").as_str() {
        "star" => NodeRole::Star,
        "planet" => NodeRole::Planet,
        _ => return Err("invalid role".into()),
    };
    let seconds: f64 = take(&mut options, "--seconds", "2").parse()?;
    if !seconds.is_finite() || !(0.1..=600.0).contains(&seconds) {
        return Err("seconds must be 0.1..600".into());
    }
    let auth = Arc::new(auth::Auth::fixture(&fixtures, role, mode == "serve", grpc)?);
    match mode.as_str() {
        "serve" => {
            let stop = tokio_util::sync::CancellationToken::new();
            let hub = match take(&mut options, "--scenario", "echo").as_str() {
                "echo" => None,
                "push" => Some(push::Hub::new(
                    push::Config {
                        recipients: take(&mut options, "--fanout", "1").parse()?,
                        rate: take(&mut options, "--rate", "2000").parse()?,
                        seconds: take(&mut options, "--push-seconds", "3").parse()?,
                        burst: take(&mut options, "--burst", "false").parse()?,
                        shape: shape(&mut options)?,
                        mode: match take(&mut options, "--data-mode", "mixed").as_str() {
                            "mixed" => push::Mode::Mixed,
                            "registry" => push::Mode::Registry,
                            "catalog" => push::Mode::Catalog,
                            _ => return Err("invalid data mode".into()),
                        },
                    },
                    stop.clone(),
                )?),
                _ => return Err("invalid scenario".into()),
            };
            if role != NodeRole::Star || !options.is_empty() {
                return Err("server requires star role and known options".into());
            }
            let listener = tokio::net::TcpListener::bind(address).await?;
            println!("{}", serde_json::json!({"ready": true, "address": listener.local_addr()?.to_string(), "pid": std::process::id()}));
            let timer = stop.clone();
            let task = tokio::spawn(async move {
                tokio::time::sleep(Duration::from_secs_f64(seconds)).await;
                timer.cancel();
            });
            let result =
                if hub.is_some() { transport::serve_push(listener, auth, grpc, stop, hub).await } else { transport::serve(listener, auth, grpc, stop).await };
            task.abort();
            result?;
        }
        "push" => {
            let fanout = take(&mut options, "--fanout", "1").parse()?;
            let pause_ms = take(&mut options, "--pause-ms", "0").parse()?;
            let shape = shape(&mut options)?;
            if !options.is_empty() || !(1..=64).contains(&fanout) || pause_ms > 1000 || !(0.2..=30.0).contains(&seconds) {
                return Err("invalid push load".into());
            }
            let report = push_measure::run(auth, push_measure::Load { address, grpc, fanout, seconds, pause_ms, shape }).await?;
            println!("{report}");
        }
        "run" => {
            let bytes = take(&mut options, "--bytes", "1024").parse()?;
            let fanout = take(&mut options, "--fanout", "1").parse()?;
            let window = take(&mut options, "--window", "64").parse()?;
            let rate = take(&mut options, "--rate", "0").parse()?;
            if !options.is_empty()
                || !(1..=16384).contains(&bytes)
                || !(1..=64).contains(&fanout)
                || !(1..=64).contains(&window)
                || (rate != 0 && rate < fanout as u64)
                || rate > 1000000
            {
                return Err("invalid load limits or unknown option".into());
            }
            let report = measure::run(auth, measure::Load { address, grpc, bytes, fanout, window, seconds, rate }).await?;
            println!("{report}");
        }
        _ => return Err(io::Error::other("expected serve or run").into()),
    }
    Ok(())
}

fn take(options: &mut BTreeMap<String, String>, name: &str, default: &str) -> String {
    options.remove(name).unwrap_or_else(|| default.to_owned())
}

fn shape(options: &mut BTreeMap<String, String>) -> Result<push_state::Shape, Box<dyn std::error::Error>> {
    let shape = push_state::Shape {
        registries: take(options, "--registries", "1000").parse()?,
        catalogs: take(options, "--catalogs", "1000").parse()?,
        bytes: take(options, "--catalog-bytes", "256").parse()?,
        hot_keys: take(options, "--hot-keys", "0").parse()?,
    };
    shape.validate()?;
    Ok(shape)
}
