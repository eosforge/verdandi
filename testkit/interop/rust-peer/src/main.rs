use std::error::Error as StdError;
use std::io::Write;
use std::time::{Duration, Instant};

use tokio::io::{AsyncBufReadExt, BufReader};
use verdandi::catalog::{
    Client as CatalogClient, Config as CatalogConfig, Entry as CatalogEntry, Kind, Path as CatalogPath, Publisher as CatalogPublisher,
    Snapshot as CatalogSnapshot, Status, Subscriber as CatalogSubscriber, Subscription as CatalogSubscription,
};
use verdandi::registration::{
    Candidate, Client as RegistrationClient, Config as RegistrationConfig, Registration as TypedRegistration, RegistrationOptions, Selector as TypedSelector,
    SelectorOptions,
};
use verdandi::{Client, Code, Config, Fields};

type Registration = TypedRegistration<Fields, Fields>;
type Selector = TypedSelector<Fields, Fields>;

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

async fn run() -> Result<(), Box<dyn StdError>> {
    let arguments: Vec<_> = std::env::args().collect();
    if arguments.len() != 3 {
        return Err("usage: rust-peer <redis-url> <zone>".into());
    }
    let mut config = Config::new(&arguments[1]);
    config.timeout = Duration::from_secs(5);
    let transport = Client::open(config).await?;
    let mut registration_config = RegistrationConfig::new(&arguments[2]);
    registration_config.selector_publish_interval = Duration::from_millis(1);
    registration_config.clock_refresh = Duration::from_secs(1);
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let selector = Selector::new(
        &client,
        SelectorOptions {
            type_name: "interop".to_owned(),
        },
    )
    .await?;
    let catalog_client = CatalogClient::open(&transport, CatalogConfig::new(&arguments[2])).await?;
    let catalog_publisher = CatalogPublisher::new(&catalog_client)?;
    let catalog_path = CatalogPath::new("interop", "shared")?;
    let catalog_subscriber = CatalogSubscriber::new(
        &catalog_client,
        CatalogSubscription {
            paths: vec![catalog_path.clone()],
            ..CatalogSubscription::default()
        },
    )
    .await?;
    let catalog_entry = catalog_subscriber.find(&catalog_path).ok_or("covered Catalog Path is missing")?;
    output("READY")?;

    let mut registration: Option<Registration> = None;
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Some(line) = lines.next_line().await? {
        let fields: Vec<_> = line.split_whitespace().collect();
        let Some(command) = fields.first() else {
            continue;
        };
        match *command {
            "PRODUCE" => {
                if registration.is_some() {
                    return Err("Rust Registration already exists".into());
                }
                let created = Registration::new(
                    &client,
                    RegistrationOptions {
                        type_name: "interop".to_owned(),
                        ttl: Duration::from_secs(30),
                        renew_interval: Some(Duration::from_secs(5)),
                        version: 21,
                    },
                )?;
                created
                    .register(
                        &fields_map([("language", b"rust".as_slice())]),
                        &fields_map([("payload", [255, 0, 2].as_slice())]),
                    )
                    .await?;
                created.update_content(22, &fields_map([("payload", [255, 0, 2, 3].as_slice())])).await?;
                output(&format!("RUST {}", created.uuid()))?;
                registration = Some(created);
            }
            "VERIFY_GO" => {
                let uuid = fields.get(1).ok_or("VERIFY_GO requires UUID")?;
                wait_record(&selector, uuid, |record| {
                    record.meta.revision == 2 && record.meta.version == 12 && record.attr["language"] == b"go" && record.data["payload"] == [0, 255, 1, 2]
                })
                .await?;
                output("VERIFIED_GO")?;
            }
            "VERIFY_RUST" => {
                let uuid = fields.get(1).ok_or("VERIFY_RUST requires UUID")?;
                wait_record(&selector, uuid, |record| {
                    record.meta.revision == 2 && record.meta.version == 22 && record.attr["language"] == b"rust" && record.data["payload"] == [255, 0, 2, 3]
                })
                .await?;
                output("VERIFIED_RUST")?;
            }
            "VERIFY_CATALOG_GO" => {
                wait_catalog(&catalog_entry, |snapshot| {
                    snapshot.synchronized
                        && snapshot.revision == 1
                        && snapshot.status == Status::Present
                        && snapshot
                            .value
                            .as_ref()
                            .is_some_and(|value| value["go"] == [0, 255, 1] && value["shared"] == b"go")
                })
                .await?;
                output("VERIFIED_CATALOG_GO")?;
            }
            "CATALOG_PRODUCE" => {
                let result = catalog_publisher
                    .replace(
                        &catalog_path,
                        Kind::Map,
                        &fields_map([("rust", [255, 0, 2].as_slice()), ("shared", b"rust".as_slice())]),
                    )
                    .await?;
                output(&format!("CATALOG_RUST {}", result.revision))?;
            }
            "VERIFY_CATALOG_DELETE" => {
                wait_catalog(&catalog_entry, |snapshot| {
                    snapshot.synchronized && snapshot.revision == 3 && snapshot.status == Status::Deleted && snapshot.value.is_none()
                })
                .await?;
                output("VERIFIED_CATALOG_DELETE")?;
            }
            "STOP" => {
                if let Some(registration) = &registration {
                    registration.close().await?;
                }
                selector.close().await?;
                catalog_subscriber.close().await?;
                catalog_client.close().await?;
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

async fn wait_record<F>(selector: &Selector, uuid: &str, predicate: F) -> Result<(), Box<dyn StdError>>
where
    F: Fn(&Candidate<Fields, Fields>) -> bool,
{
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match selector.find(uuid).await {
            Ok(Some(record)) if predicate(&record) => return Ok(()),
            Ok(_) => {}
            Err(error) if error.code() == Code::Unavailable => {}
            Err(error) => return Err(error.into()),
        }
        if Instant::now() >= deadline {
            return Err(format!("Rust Selector did not observe {uuid}").into());
        }
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
}

async fn wait_catalog<F>(entry: &CatalogEntry, predicate: F) -> Result<(), Box<dyn StdError>>
where
    F: Fn(&CatalogSnapshot<Fields>) -> bool,
{
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let snapshot = entry.load::<Fields>()?;
        if predicate(&snapshot) {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err("Rust Catalog Subscriber did not observe expected state".into());
        }
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
}

fn fields_map<const N: usize>(values: [(&str, &[u8]); N]) -> Fields {
    values.into_iter().map(|(name, value)| (name.to_owned(), value.to_vec())).collect()
}

fn output(value: &str) -> std::io::Result<()> {
    println!("{value}");
    std::io::stdout().flush()
}
