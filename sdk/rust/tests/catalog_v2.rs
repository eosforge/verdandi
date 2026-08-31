use std::collections::BTreeMap;
use std::time::{Duration, Instant};

use fred::prelude::*;
use fred::types::Builder;
use fred::types::scan::Scanner;
use futures_util::StreamExt;
use verdandi::catalog::{Client as CatalogClient, Config, Kind, Patch, Path, Publisher, Status, Subscriber, Subscription};
use verdandi::{Client, Code, Config as TransportConfig, Error, FieldValue, Fields, Result};

#[derive(Debug, Eq, PartialEq)]
struct Banner(String);

impl FieldValue for Banner {
    fn encode_fields(&self, destination: &mut Fields) -> Result<()> {
        destination.insert("value".to_owned(), self.0.as_bytes().to_vec());
        Ok(())
    }

    fn decode_fields(source: &Fields) -> Result<Self> {
        let value = source.get("value").ok_or_else(|| Error::field(Code::Corrupt, "value"))?;
        String::from_utf8(value.clone())
            .map(Self)
            .map_err(|error| Error::field(Code::Corrupt, error.to_string()))
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn publisher_subscriber_shapes_patch_delete_and_generic_load() -> Result<()> {
    let endpoint = match std::env::var("VERDANDI_REDIS_URL") {
        Ok(endpoint) if !endpoint.trim().is_empty() => endpoint,
        _ => return Ok(()),
    };
    let zone = integration_zone();
    let fred_config = fred::types::config::Config::from_url(&endpoint).map_err(|error| Error::field(Code::Invalid, error.to_string()))?;
    let raw = Builder::from_config(fred_config)
        .build()
        .map_err(|error| Error::field(Code::Invalid, error.to_string()))?;
    raw.init().await.map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;

    let transport = Client::open(TransportConfig::new(&endpoint)).await?;
    let client = CatalogClient::open(&transport, Config::new(&zone)).await?;
    let publisher = Publisher::new(&client)?;
    let route_path = Path::new("routing", "public")?;
    let banner_path = Path::new("routing", "banner")?;
    let subscriber = Subscriber::new(
        &client,
        Subscription {
            parts: vec!["routing".to_owned()],
            ..Subscription::default()
        },
    )
    .await?;

    let route = subscriber.find(&route_path).ok_or_else(|| Error::field(Code::Missing, "route"))?;
    assert_eq!(route.status(), Status::Absent);
    let created = publisher
        .replace(
            &route_path,
            Kind::Map,
            &Fields::from([("primary".to_owned(), b"east".to_vec()), ("weight".to_owned(), b"10".to_vec())]),
        )
        .await?;
    wait_state(&route, Status::Present, created.revision).await?;
    let patched = publisher
        .patch(
            &route_path,
            Patch {
                base_revision: created.revision,
                set: Fields::from([("primary".to_owned(), b"west".to_vec()), ("zone".to_owned(), b"secondary".to_vec())]),
            },
        )
        .await?;
    wait_state(&route, Status::Present, patched.revision).await?;
    let raw_route = route.load::<Fields>()?;
    assert_eq!(raw_route.value.as_ref().and_then(|value| value.get("primary")), Some(&b"west".to_vec()));

    let banner_revision = publisher.replace(&banner_path, Kind::Value, &Banner("ready".to_owned())).await?;
    let banner = subscriber.find(&banner_path).ok_or_else(|| Error::field(Code::Missing, "banner"))?;
    wait_state(&banner, Status::Present, banner_revision.revision).await?;
    assert_eq!(banner.load::<Banner>()?.value, Some(Banner("ready".to_owned())));

    let deleted = publisher.delete(&route_path).await?;
    wait_state(&route, Status::Deleted, deleted.revision).await?;
    assert!(subscriber.find(&route_path).is_some_and(|entry| entry.revision() == deleted.revision));

    subscriber.close().await?;
    assert!(subscriber.find(&banner_path).is_none());
    let closed_banner = banner.load::<Banner>()?;
    assert_eq!(closed_banner.status, Status::Closed);
    assert!(!closed_banner.synchronized);
    assert_eq!(closed_banner.value, Some(Banner("ready".to_owned())));
    client.close().await?;
    transport.close().await?;
    cleanup_zone(&raw, &zone).await?;
    raw.quit().await.map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread")]
async fn checkpoint_restarts_with_zone_delta_and_field_patch() -> Result<()> {
    let endpoint = match std::env::var("VERDANDI_REDIS_URL") {
        Ok(endpoint) if !endpoint.trim().is_empty() => endpoint,
        _ => return Ok(()),
    };
    let zone = integration_zone();
    let store_path = std::env::temp_dir().join(format!("verdandi-{zone}.redb"));
    let fred_config = fred::types::config::Config::from_url(&endpoint).map_err(|error| Error::field(Code::Invalid, error.to_string()))?;
    let raw = Builder::from_config(fred_config)
        .build()
        .map_err(|error| Error::field(Code::Invalid, error.to_string()))?;
    raw.init().await.map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;

    let transport = Client::open(TransportConfig::new(&endpoint)).await?;
    let mut config = Config::new(&zone);
    config.local_store_path = Some(store_path.clone());
    let client = CatalogClient::open(&transport, config).await?;
    let publisher = Publisher::new(&client)?;
    let path = Path::new("routing", "checkpoint")?;
    let created = publisher
        .replace(&path, Kind::Map, &Fields::from([("target".to_owned(), b"east".to_vec())]))
        .await?;
    let subscription = Subscription {
        parts: vec!["routing".to_owned()],
        ..Subscription::default()
    };
    let first = Subscriber::new(&client, subscription.clone()).await?;
    let first_entry = first.find(&path).ok_or_else(|| Error::field(Code::Missing, "checkpoint"))?;
    wait_state(&first_entry, Status::Present, created.revision).await?;
    let live = publisher
        .patch(
            &path,
            Patch {
                base_revision: created.revision,
                set: Fields::from([("target".to_owned(), b"west".to_vec())]),
            },
        )
        .await?;
    wait_state(&first_entry, Status::Present, live.revision).await?;
    first.close().await?;
    drop(first);

    let offline = publisher
        .patch(
            &path,
            Patch {
                base_revision: live.revision,
                set: Fields::from([("target".to_owned(), b"north".to_vec())]),
            },
        )
        .await?;
    let second = Subscriber::new(&client, subscription.clone()).await?;
    let second_entry = second.find(&path).ok_or_else(|| Error::field(Code::Missing, "checkpoint"))?;
    wait_state(&second_entry, Status::Present, offline.revision).await?;
    assert_eq!(
        second_entry.load::<Fields>()?.value.and_then(|value| value.get("target").cloned()),
        Some(b"north".to_vec())
    );

    second.close().await?;
    drop(second);

    // Model a legally evicted tombstone by advancing the floor beyond the
    // persisted cursor. Reopening must full-align instead of trusting a partial
    // ZSET delta.
    let evicted_path = Path::new("routing", "evicted")?;
    publisher
        .replace(&evicted_path, Kind::Map, &Fields::from([("target".to_owned(), b"temporary".to_vec())]))
        .await?;
    let evicted = publisher.delete(&evicted_path).await?;
    let member = "routing:evicted".to_owned();
    let _: i64 = raw
        .zrem(format!("verdandi:catalog:{zone}:@deleted"), vec![member.clone()])
        .await
        .map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
    let _: i64 = raw
        .zrem(format!("verdandi:catalog:{zone}:@deleted_time"), vec![member])
        .await
        .map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
    let _: i64 = raw
        .hset(
            format!("verdandi:catalog:{zone}:@meta"),
            BTreeMap::from([("@floor_revision".to_owned(), evicted.revision.to_string())]),
        )
        .await
        .map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
    let current = publisher
        .patch(
            &path,
            Patch {
                base_revision: offline.revision,
                set: Fields::from([("target".to_owned(), b"full-alignment".to_vec())]),
            },
        )
        .await?;
    let third = Subscriber::new(&client, subscription).await?;
    let third_entry = third.find(&path).ok_or_else(|| Error::field(Code::Missing, "checkpoint"))?;
    wait_state(&third_entry, Status::Present, current.revision).await?;
    assert_eq!(
        third_entry.load::<Fields>()?.value.and_then(|value| value.get("target").cloned()),
        Some(b"full-alignment".to_vec())
    );
    assert!(third.find(&evicted_path).is_some_and(|entry| entry.status() == Status::Absent));

    third.close().await?;
    client.close().await?;
    transport.close().await?;
    drop(third);
    drop(publisher);
    drop(client);
    drop(transport);
    cleanup_zone(&raw, &zone).await?;
    raw.quit().await.map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
    std::fs::remove_file(store_path).map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
    Ok(())
}

async fn wait_state(entry: &verdandi::catalog::Entry, status: Status, revision: u64) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        if entry.status() == status && entry.revision() == revision {
            return Ok(());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    Err(Error::field(Code::Deadline, "catalog_state"))
}

fn integration_zone() -> String {
    let suffix = (0..12).map(|_| char::from(b'a' + fastrand::u8(0..26))).collect::<String>();
    format!("Catalog{suffix}")
}

async fn cleanup_zone(client: &fred::clients::Client, zone: &str) -> Result<()> {
    let scanner = client.scan(format!("verdandi:catalog:{zone}:*"), Some(256), None);
    tokio::pin!(scanner);
    let mut keys = Vec::new();
    while let Some(page) = scanner.as_mut().next().await {
        let mut page = page.map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
        let values = page.take_results().ok_or_else(|| Error::field(Code::Corrupt, "scan"))?;
        for value in values {
            keys.push(value.into_string().ok_or_else(|| Error::field(Code::Corrupt, "scan"))?);
        }
    }
    if !keys.is_empty() {
        let _: i64 = client.unlink(keys).await.map_err(|error| Error::field(Code::Unavailable, error.to_string()))?;
    }
    Ok(())
}
