use std::error::Error as StdError;
use std::io::Write;
use std::time::{Duration, Instant};

use tokio::io::{AsyncBufReadExt, BufReader};
use verdandi::catalog::{Client as CatalogClient, Config as CatalogConfig, Entry, Kind, Patch, Path, Publisher, Snapshot, Status, Subscriber, Subscription};
use verdandi::{Client, Config, Fields};

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

async fn run() -> Result<(), Box<dyn StdError>> {
    let arguments: Vec<_> = std::env::args().collect();
    if arguments.len() != 2 {
        return Err("usage: catalog-rust-peer <zone>".into());
    }
    let endpoint = std::env::var("VERDANDI_CATALOG_ENDPOINT")?;
    let mut config = Config::new(endpoint);
    config.timeout = Duration::from_secs(5);
    let transport = Client::open(config).await?;
    let mut catalog_config = CatalogConfig::new(&arguments[1]);
    catalog_config.sync_timeout = Duration::from_secs(30);
    let client = CatalogClient::open(&transport, catalog_config).await?;
    let publisher = Publisher::new(&client)?;
    let path = Path::new("interop", "shared")?;
    let subscriber = Subscriber::new(
        &client,
        Subscription {
            paths: vec![path.clone()],
            ..Subscription::default()
        },
    )
    .await?;
    let entry = subscriber.find(&path).ok_or("covered Catalog Path is missing")?;
    output("READY")?;

    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Some(line) = lines.next_line().await? {
        let fields: Vec<_> = line.split_whitespace().collect();
        let Some(command) = fields.first() else {
            continue;
        };
        match *command {
            "REPLACE" => {
                let owner = fields.get(1).ok_or("REPLACE requires owner")?;
                let generation = fields.get(2).ok_or("REPLACE requires generation")?;
                match publisher.replace(&path, Kind::Map, &peer_fields(owner, generation)).await {
                    Ok(result) => output(&format!("REVISION {}", result.revision))?,
                    Err(error) => output(&format!("ERROR {error}"))?,
                }
            }
            "PATCH" => {
                let base = fields.get(1).ok_or("PATCH requires base revision")?.parse::<u64>()?;
                let owner = fields.get(2).ok_or("PATCH requires owner")?;
                let generation = fields.get(3).ok_or("PATCH requires generation")?;
                match publisher
                    .patch(
                        &path,
                        Patch {
                            base_revision: base,
                            set: peer_fields(owner, generation),
                        },
                    )
                    .await
                {
                    Ok(result) => output(&format!("REVISION {}", result.revision))?,
                    Err(error) => output(&format!("ERROR {error}"))?,
                }
            }
            "DELETE" => match publisher.delete(&path).await {
                Ok(result) => output(&format!("REVISION {}", result.revision))?,
                Err(error) => output(&format!("ERROR {error}"))?,
            },
            "CHECK" => {
                let revision = fields.get(1).ok_or("CHECK requires revision")?.parse::<u64>()?;
                let owner = fields.get(2).ok_or("CHECK requires owner")?;
                let generation = fields.get(3).ok_or("CHECK requires generation")?;
                match wait_present(&entry, revision, owner, generation).await {
                    Ok(()) => output("CHECKED")?,
                    Err(error) => output(&format!("ERROR {error}"))?,
                }
            }
            "CHECK_DELETED" => {
                let revision = fields.get(1).ok_or("CHECK_DELETED requires revision")?.parse::<u64>()?;
                match wait_deleted(&entry, revision).await {
                    Ok(()) => output("CHECKED")?,
                    Err(error) => output(&format!("ERROR {error}"))?,
                }
            }
            "STOP" => {
                subscriber.close().await?;
                client.close().await?;
                transport.close().await?;
                output("STOPPED")?;
                return Ok(());
            }
            _ => return Err(format!("unknown command {command:?}").into()),
        }
    }
    Err("stdin closed before STOP".into())
}

fn peer_fields(owner: &str, generation: &str) -> Fields {
    Fields::from([
        ("owner".to_owned(), owner.as_bytes().to_vec()),
        ("generation".to_owned(), generation.as_bytes().to_vec()),
        ("binary".to_owned(), vec![0, 255, 1, 254]),
    ])
}

async fn wait_present(entry: &Entry, revision: u64, owner: &str, generation: &str) -> Result<(), Box<dyn StdError>> {
    wait(entry, |snapshot| {
        snapshot.synchronized
            && snapshot.status == Status::Present
            && snapshot.revision == revision
            && snapshot.value.as_ref().is_some_and(|value| {
                value.get("owner").is_some_and(|value| value == owner.as_bytes())
                    && value.get("generation").is_some_and(|value| value == generation.as_bytes())
                    && value.get("binary").is_some_and(|value| value == &[0, 255, 1, 254])
            })
    })
    .await
}

async fn wait_deleted(entry: &Entry, revision: u64) -> Result<(), Box<dyn StdError>> {
    wait(entry, |snapshot| {
        snapshot.synchronized && snapshot.status == Status::Deleted && snapshot.revision == revision && snapshot.value.is_none()
    })
    .await
}

async fn wait<F>(entry: &Entry, predicate: F) -> Result<(), Box<dyn StdError>>
where
    F: Fn(&Snapshot<Fields>) -> bool,
{
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        let snapshot = entry.load::<Fields>()?;
        if predicate(&snapshot) {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err("Catalog state did not converge".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

fn output(value: &str) -> std::io::Result<()> {
    println!("{value}");
    std::io::stdout().flush()
}
