use std::time::Instant;

use fred::types::Builder;
use fred::types::client::ClientKillFilter;

use super::super::config::Config;
use super::super::model::mutation_keys;
use super::super::publisher::Publisher;
use super::*;
use std::time::Duration;

#[test]
fn synchronization_slot_does_not_lose_requests() {
    let mut state = SyncState {
        running: true,
        ..SyncState::default()
    };
    state.batch.scope = true;
    let first = state.take_or_stop();
    assert!(first.is_some());
    assert!(state.running);

    let path = match Path::new("routing", "queued") {
        Ok(path) => path,
        Err(error) => panic!("path construction failed: {error}"),
    };
    state.batch.paths.insert(path);
    let second = state.take_or_stop();
    assert!(second.is_some());
    assert!(state.running);

    assert!(state.take_or_stop().is_none());
    assert!(!state.running);
    state.batch.scope = true;
    if !state.running {
        state.running = true;
    }
    assert!(state.running);
    assert!(state.take_or_stop().is_some());
}

#[test]
fn internal_catalog_locks_recover_after_poison() {
    let mutex = std::sync::Arc::new(std::sync::Mutex::new(1_u8));
    let poisoned_mutex = std::sync::Arc::clone(&mutex);
    assert!(
        std::thread::spawn(move || {
            let mut value = match poisoned_mutex.lock() {
                Ok(value) => value,
                Err(error) => error.into_inner(),
            };
            *value = 2;
            panic!("poison mutex");
        })
        .join()
        .is_err()
    );
    *mutex_lock(&mutex) = 3;
    assert_eq!(*mutex_lock(&mutex), 3);

    let rwlock = std::sync::Arc::new(std::sync::RwLock::new(4_u8));
    let poisoned_rwlock = std::sync::Arc::clone(&rwlock);
    assert!(
        std::thread::spawn(move || {
            let mut value = match poisoned_rwlock.write() {
                Ok(value) => value,
                Err(error) => error.into_inner(),
            };
            *value = 5;
            panic!("poison rwlock");
        })
        .join()
        .is_err()
    );
    *write_lock(&rwlock) = 6;
    assert_eq!(*read_lock(&rwlock), 6);
}

#[test]
fn notification_parser_is_bounded_and_rejects_trailing_data() {
    let path = match Path::new("routing", "parser") {
        Ok(path) => path,
        Err(error) => panic!("path construction failed: {error}"),
    };
    let value = rmpv::Value::Array(vec![
        rmpv::Value::from("v1"),
        rmpv::Value::from("replace"),
        rmpv::Value::from(path.member()),
        rmpv::Value::from("1"),
        rmpv::Value::from("map"),
        rmpv::Value::from("4"),
        rmpv::Value::Array(vec![rmpv::Value::from("a"), rmpv::Value::Binary(b"one".to_vec())]),
    ]);
    let mut payload = Vec::new();
    if let Err(error) = rmpv::encode::write_value(&mut payload, &value) {
        panic!("notification encoding failed: {error}");
    }
    let event = match decode_event(&payload, &path, 128) {
        Ok(event) => event,
        Err(error) => panic!("notification decoding failed: {error}"),
    };
    assert_eq!(event.revision, 1);
    assert_eq!(event.fields.get("a"), Some(&b"one".to_vec()));
    payload.push(0);
    assert!(decode_event(&payload, &path, 128).is_err());

    let impossible_array = [0xdd, 0xff, 0xff, 0xff, 0xff];
    assert!(decode_event(&impossible_array, &path, 128).is_err());

    let prefix = rmpv::Value::Array(vec![
        rmpv::Value::from("v1"),
        rmpv::Value::from("replace"),
        rmpv::Value::from(path.member()),
        rmpv::Value::from("1"),
        rmpv::Value::from("map"),
        rmpv::Value::from("0"),
    ]);
    let mut impossible_fields = Vec::new();
    if let Err(error) = rmpv::encode::write_value(&mut impossible_fields, &prefix) {
        panic!("notification prefix encoding failed: {error}");
    }
    let Some(marker) = impossible_fields.first_mut() else {
        panic!("notification prefix is empty");
    };
    *marker = 0x97;
    impossible_fields.extend_from_slice(&[0xdd, 0xff, 0xff, 0xff, 0xff]);
    assert!(decode_event(&impossible_fields, &path, 128).is_err());

    let non_canonical_map = rmpv::Value::Array(vec![
        rmpv::Value::from("v1"),
        rmpv::Value::from("replace"),
        rmpv::Value::from(path.member()),
        rmpv::Value::from("2"),
        rmpv::Value::from("map"),
        rmpv::Value::from("3"),
        rmpv::Value::Array(vec![
            rmpv::Value::from("9"),
            rmpv::Value::Binary(Vec::new()),
            rmpv::Value::from("10"),
            rmpv::Value::Binary(Vec::new()),
        ]),
    ]);
    let mut non_canonical_payload = Vec::new();
    if let Err(error) = rmpv::encode::write_value(&mut non_canonical_payload, &non_canonical_map) {
        panic!("notification encoding failed: {error}");
    }
    assert!(decode_event(&non_canonical_payload, &path, 128).is_err());
}

#[tokio::test(flavor = "multi_thread")]
async fn pubsub_reconnect_resubscribes_and_repairs_missed_replace() -> Result<()> {
    let endpoint = match std::env::var("VERDANDI_REDIS_URL") {
        Ok(endpoint) if !endpoint.trim().is_empty() => endpoint,
        _ => return Ok(()),
    };
    let suffix = (0..12).map(|_| char::from(b'a' + fastrand::u8(0..26))).collect::<String>();
    let zone = format!("Catalog{suffix}");
    let fred_config = fred::types::config::Config::from_url(&endpoint).map_err(|error| Error::driver(Code::Invalid, error))?;
    let raw = Builder::from_config(fred_config).build().map_err(|error| Error::driver(Code::Invalid, error))?;
    raw.init().await.map_err(|error| Error::driver(Code::Unavailable, error))?;

    let transport = crate::Client::open(crate::Config::new(&endpoint)).await?;
    let client = Client::open(&transport, Config::new(&zone)).await?;
    let publisher = Publisher::new(&client)?;
    let path = Path::new("routing", "reconnect")?;
    let subscriber = Subscriber::new(
        &client,
        Subscription {
            parts: vec!["routing".to_owned()],
            ..Subscription::default()
        },
    )
    .await?;
    wait_catalog_worker_count(&subscriber, 1).await?;
    let entry = subscriber.find(&path).ok_or_else(|| Error::field(Code::Missing, "reconnect"))?;
    let connection_ids = subscriber.inner.pubsub.connection_ids();
    if connection_ids.len() != 1 {
        return Err(Error::field(Code::Corrupt, "pubsub_connection"));
    }
    let connection_id = connection_ids
        .values()
        .next()
        .copied()
        .ok_or_else(|| Error::field(Code::Corrupt, "pubsub_connection"))?;
    let _: i64 = raw
        .client_kill(vec![ClientKillFilter::ID(connection_id.to_string())])
        .await
        .map_err(|error| Error::driver(Code::Unavailable, error))?;

    let replaced = publisher
        .replace(&path, Kind::Map, &Fields::from([("target".to_owned(), b"recovered".to_vec())]))
        .await?;
    let deadline = Instant::now() + Duration::from_secs(10);
    while entry.revision() != replaced.revision || entry.status() != Status::Present {
        if Instant::now() >= deadline {
            return Err(Error::field(Code::Deadline, "reconnect"));
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    wait_catalog_worker_count(&subscriber, 1).await?;

    subscriber.close().await?;
    if subscriber.inner.workers.load(Ordering::Acquire) != 0 {
        return Err(Error::field(Code::Corrupt, "catalog_workers"));
    }
    client.close().await?;
    transport.close().await?;
    let _: i64 = raw
        .unlink(mutation_keys(&zone, &path))
        .await
        .map_err(|error| Error::driver(Code::Unavailable, error))?;
    raw.quit().await.map_err(|error| Error::driver(Code::Unavailable, error))?;
    Ok(())
}

async fn wait_catalog_worker_count(subscriber: &Subscriber, expected: usize) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if subscriber.inner.workers.load(Ordering::Acquire) == expected {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err(Error::field(Code::Deadline, "catalog_workers"));
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}
