use std::error::Error as StdError;
use std::io::Write;
use std::time::{Duration, Instant};

use tokio::io::{AsyncBufReadExt, BufReader};
use verdandi::registration::{
    Client as RegistrationClient, Config as RegistrationConfig, Registration as TypedRegistration, RegistrationOptions, Selector as TypedSelector,
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
    let zone = std::env::args().nth(1).ok_or("usage: rust-peer <zone>")?;
    let endpoint = std::env::var("VERDANDI_SENTINEL_URL")?;
    let mut config = Config::new(endpoint);
    config.timeout = Duration::from_secs(3);
    let transport = Client::open(config).await?;
    let mut registration_config = RegistrationConfig::new(zone);
    registration_config.selector_page_size = 64;
    registration_config.selector_event_buffer = 1024;
    registration_config.selector_event_bytes = 16 * 1024 * 1024;
    registration_config.selector_publish_interval = Duration::from_millis(1);
    registration_config.selector_sync_timeout = Duration::from_secs(20);
    registration_config.clock_refresh = Duration::from_secs(1);
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let selector = Selector::new(&client, SelectorOptions { type_name: "fault".to_owned() }).await?;
    let registration = Registration::new(
        &client,
        RegistrationOptions {
            type_name: "fault".to_owned(),
            ttl: Duration::from_secs(5 * 60),
            renew_interval: Some(Duration::from_secs(60)),
            version: 1,
        },
    )?;
    registration
        .register(&fields([("language", b"rust".as_slice())]), &fields([("value", b"initial".as_slice())]))
        .await?;
    output(&format!("READY {}", registration.uuid()))?;
    let mut desired = "initial".to_owned();

    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Some(line) = lines.next_line().await? {
        let parts: Vec<_> = line.split_whitespace().collect();
        let Some(command) = parts.first() else {
            continue;
        };
        match *command {
            "UPDATE" => {
                let value = parts.get(1).ok_or("UPDATE requires one value")?;
                let result = registration.update(&fields([("value", value.as_bytes())])).await;
                let outcome = match result {
                    Ok(()) => "ok",
                    Err(error) if error.code() == Code::Ambiguous => "ambiguous",
                    Err(error) => return Err(error.into()),
                };
                wait_record(&selector, registration.uuid(), value, Some(registration.revision())).await?;
                desired = (*value).to_owned();
                output(&format!(
                    "UPDATED {} {} {} {outcome}",
                    registration.uuid(),
                    registration.revision(),
                    registration.timestamp()
                ))?;
            }
            "RENEW" => {
                renew_until_confirmed(&registration).await?;
                wait_record(&selector, registration.uuid(), &desired, Some(registration.revision())).await?;
                output(&format!(
                    "RENEWED {} {} {}",
                    registration.uuid(),
                    registration.revision(),
                    registration.timestamp()
                ))?;
            }
            "CHECK" => {
                let uuid = parts.get(1).ok_or("CHECK requires UUID and value")?;
                let value = parts.get(2).ok_or("CHECK requires UUID and value")?;
                let (revision, generation) = wait_record(&selector, uuid, value, None).await?;
                output(&format!("CHECKED {uuid} {revision} {generation}"))?;
            }
            "WAIT_UNSYNC" => {
                let deadline = Instant::now() + Duration::from_secs(15);
                let mut generation = 0;
                loop {
                    match selector.snapshot().await {
                        Ok(snapshot) => generation = snapshot.generation,
                        Err(error) if error.code() == Code::Unavailable => break,
                        Err(error) => return Err(error.into()),
                    }
                    if Instant::now() >= deadline {
                        return Err("Selector remained synchronized".into());
                    }
                    tokio::time::sleep(Duration::from_millis(10)).await;
                }
                output(&format!("UNSYNCHRONIZED {generation}"))?;
            }
            "STOP" => {
                registration.close().await?;
                selector.close().await?;
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

async fn renew_until_confirmed(registration: &Registration) -> Result<(), Box<dyn StdError>> {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        match registration.renew().await {
            Ok(()) => return Ok(()),
            Err(error) if matches!(error.code(), Code::Ambiguous | Code::Unavailable | Code::Deadline) && Instant::now() < deadline => {
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
            Err(error) => return Err(error.into()),
        }
    }
}

async fn wait_record(selector: &Selector, uuid: &str, value: &str, revision: Option<u64>) -> Result<(u64, u64), Box<dyn StdError>> {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        match selector.snapshot().await {
            Ok(snapshot) => match selector.find(uuid).await {
                Ok(Some(record))
                    if record.data.get("value").map(Vec::as_slice) == Some(value.as_bytes())
                        && revision.is_none_or(|expected| record.meta.revision == expected) =>
                {
                    return Ok((record.meta.revision, snapshot.generation));
                }
                Ok(_) => {}
                Err(error) if error.code() == Code::Unavailable => {}
                Err(error) => return Err(error.into()),
            },
            Err(error) if error.code() == Code::Unavailable => {}
            Err(error) => return Err(error.into()),
        }
        if Instant::now() >= deadline {
            return Err(format!("Selector did not converge for {uuid}").into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

fn fields<const N: usize>(values: [(&str, &[u8]); N]) -> Fields {
    values.into_iter().map(|(name, value)| (name.to_owned(), value.to_vec())).collect()
}

fn output(value: &str) -> std::io::Result<()> {
    println!("{value}");
    std::io::stdout().flush()
}
