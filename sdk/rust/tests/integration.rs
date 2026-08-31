use std::collections::BTreeMap;
use std::error::Error as StdError;
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use fred::prelude::*;
use fred::types::scripts::Script;
use fred::types::{InfoKind, Value};
use tokio::task::JoinSet;
use verdandi::registration::{
    Candidate, Client as RegistrationClient, Config as RegistrationConfig, Registration, RegistrationOptions, RetainedCandidate, Selector, SelectorOptions,
};

type FieldsRegistration = Registration<Fields, Fields>;
type FieldsSelector = Selector<Fields, Fields>;
use verdandi::{Client, Code, Config, Error as VerdandiError, FieldValue, Fields};

const TYPE_NAME: &str = "Proxy";
const RAW_UUID: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

#[derive(Debug, Eq, PartialEq)]
struct ProxyAttr {
    region: String,
}

impl FieldValue for ProxyAttr {
    fn encode_fields(&self, dst: &mut Fields) -> verdandi::Result<()> {
        dst.insert("region".to_owned(), self.region.as_bytes().to_vec());
        Ok(())
    }

    fn decode_fields(src: &Fields) -> verdandi::Result<Self> {
        let region = src.get("region").ok_or_else(|| VerdandiError::field(Code::Invalid, "region"))?;
        Ok(Self {
            region: std::str::from_utf8(region)
                .map_err(|_| VerdandiError::field(Code::Invalid, "region"))?
                .to_owned(),
        })
    }
}

#[derive(Debug, Eq, PartialEq)]
struct ProxyData {
    power: i64,
    queued: i64,
}

impl FieldValue for ProxyData {
    fn encode_fields(&self, dst: &mut Fields) -> verdandi::Result<()> {
        dst.insert("power".to_owned(), self.power.to_string().into_bytes());
        dst.insert("queued".to_owned(), self.queued.to_string().into_bytes());
        Ok(())
    }

    fn decode_fields(src: &Fields) -> verdandi::Result<Self> {
        let power = src.get("power").ok_or_else(|| VerdandiError::field(Code::Invalid, "power"))?;
        let queued = src.get("queued").ok_or_else(|| VerdandiError::field(Code::Invalid, "queued"))?;
        Ok(Self {
            power: std::str::from_utf8(power)
                .map_err(|_| VerdandiError::field(Code::Invalid, "power"))?
                .parse()
                .map_err(|_| VerdandiError::field(Code::Invalid, "power"))?,
            queued: std::str::from_utf8(queued)
                .map_err(|_| VerdandiError::field(Code::Invalid, "queued"))?
                .parse()
                .map_err(|_| VerdandiError::field(Code::Invalid, "queued"))?,
        })
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn typed_registration_and_transactional_selector() -> Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let zone = unique_zone()?;
    let redis = direct_client(&endpoint).await?;
    let transport = Client::open(Config::new(&endpoint)).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_publish_interval: Duration::from_millis(1),
        clock_refresh: Duration::from_secs(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;

    let registration = Registration::<ProxyAttr, ProxyData>::new(
        &client,
        RegistrationOptions {
            type_name: TYPE_NAME.to_owned(),
            ttl: Duration::from_secs(3),
            renew_interval: Some(Duration::from_millis(500)),
            version: 1,
        },
    )?;
    assert_eq!(registration.uuid().len(), 32);
    assert!(!registration.is_registered());
    assert_eq!(registration.revision(), 0);

    let selector = Selector::<ProxyAttr, ProxyData>::new(
        &client,
        SelectorOptions {
            type_name: TYPE_NAME.to_owned(),
        },
    )
    .await?;
    registration
        .register(&ProxyAttr { region: "east".to_owned() }, &ProxyData { power: 1, queued: 0 })
        .await?;
    assert!(registration.is_registered());
    let uuid = registration.uuid().to_owned();

    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if selector.find(&uuid).await?.is_some() {
            break;
        }
        if Instant::now() >= deadline {
            return Err("typed Selector did not observe Registration".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }

    let selected = selector
        .one(Duration::from_secs(1), |candidates| {
            let choice = candidates.get(0).ok_or_else(|| VerdandiError::field(Code::Missing, "candidate"))?.choice();
            candidates.mutate(choice, |data| {
                data.power += 1;
                Ok(())
            })?;
            assert_eq!(candidates.get(0).map(|value| value.data.power), Some(2));
            Ok(Some(choice))
        })
        .await?
        .ok_or("typed One returned no candidate")?;
    assert_eq!(selected.data.power, 2);
    assert_eq!(selector.find(&uuid).await?.map(|value| value.data.power), Some(2));

    let rollback = selector
        .one(Duration::from_secs(1), |candidates| {
            let choice = candidates.get(0).ok_or_else(|| VerdandiError::field(Code::Missing, "candidate"))?.choice();
            candidates.mutate(choice, |data| {
                data.power = 99;
                Ok(())
            })?;
            Err(VerdandiError::field(Code::Contract, "policy"))
        })
        .await;
    assert!(matches!(rollback, Err(error) if error.code() == Code::Contract));
    assert_eq!(selector.find(&uuid).await?.map(|value| value.data.power), Some(2));

    let selected = selector
        .any(Duration::from_secs(1), |candidates| {
            let choice = candidates.get(0).ok_or_else(|| VerdandiError::field(Code::Missing, "candidate"))?.choice();
            candidates.mutate(choice, |data| {
                data.queued = 1;
                Ok(())
            })?;
            Ok(vec![choice])
        })
        .await?;
    assert_eq!(selected.len(), 1);
    assert_eq!(selected[0].data.queued, 1);

    let duplicate = selector
        .any(Duration::from_secs(1), |candidates| {
            let choice = candidates.get(0).ok_or_else(|| VerdandiError::field(Code::Missing, "candidate"))?.choice();
            candidates.mutate(choice, |data| {
                data.power = 99;
                Ok(())
            })?;
            Ok(vec![choice, choice])
        })
        .await;
    assert!(matches!(duplicate, Err(error) if error.code() == Code::Contract));
    assert!(
        selector
            .find(&uuid)
            .await?
            .is_some_and(|value| { value.data.power == 2 && value.data.queued == 1 })
    );

    registration.update(&ProxyData { power: 1, queued: 5 }).await?;
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if selector
            .find(&uuid)
            .await?
            .is_some_and(|value| value.meta.revision == 2 && value.data.power == 2 && value.data.queued == 5)
        {
            break;
        }
        if Instant::now() >= deadline {
            return Err("typed Selector did not merge an unrelated remote field".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }

    registration.update(&ProxyData { power: 8, queued: 5 }).await?;
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if selector
            .find(&uuid)
            .await?
            .is_some_and(|value| value.meta.revision == 3 && value.data.power == 8 && value.data.queued == 5)
        {
            break;
        }
        if Instant::now() >= deadline {
            return Err("typed Selector did not reconcile remote Update".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }

    registration.unregister().await?;
    selector.close().await?;
    client.close().await?;
    transport.close().await?;
    let _: i64 = redis.del(format!("verdandi:config:{zone}")).await?;
    redis.quit().await?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn registration_and_selector_reconcile_on_redis_8() -> Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let zone = unique_zone()?;
    let config_key = format!("verdandi:config:{zone}");
    let registry_key = format!("verdandi:registry:{zone}:{TYPE_NAME}");
    let raw_key = format!("verdandi:registration:{zone}:{TYPE_NAME}:{RAW_UUID}");
    let redis = direct_client(&endpoint).await?;

    let transport = Client::open(Config::new(&endpoint)).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_publish_interval: Duration::from_millis(1),
        clock_refresh: Duration::from_secs(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;
    assert_eq!(client.registration_limits().attr_max_fields, 16);
    assert_eq!(client.registration_limits().data_max_fields, 32);
    assert_eq!(client.registration_limits().attr_value_max_bytes, 128);
    assert_eq!(client.registration_limits().data_value_max_bytes, 128);

    let selector = FieldsSelector::new(
        &client,
        SelectorOptions {
            type_name: TYPE_NAME.to_owned(),
        },
    )
    .await?;
    assert!(selector.snapshot().await?.synchronized);
    assert!(selector.snapshot().await?.candidates.is_empty());

    let registration = Arc::new(FieldsRegistration::new(
        &client,
        RegistrationOptions {
            type_name: TYPE_NAME.to_owned(),
            ttl: Duration::from_millis(1200),
            renew_interval: Some(Duration::from_millis(300)),
            version: 1,
        },
    )?);
    let attr = fields([("role", b"worker".as_slice())]);
    let mut data = fields([("load", b"0".as_slice()), ("state", b"ready".as_slice())]);
    registration.register(&attr, &data).await?;
    let uuid = registration.uuid().to_owned();
    wait_for_candidate(&selector, &uuid, Duration::from_secs(2), |record| {
        record.meta.revision == 1 && record.attr["role"] == b"worker" && record.data["state"] == b"ready"
    })
    .await?;

    data.insert("load".to_owned(), b"1".to_vec());
    registration.update_content(2, &data).await?;
    wait_for_candidate(&selector, &uuid, Duration::from_secs(2), |record| {
        record.meta.revision == 2 && record.meta.version == 2 && record.data["load"] == b"1"
    })
    .await?;
    registration.update_content(2, &data).await?;
    assert_eq!(registration.revision(), 2);
    let timestamp = registration.timestamp();
    registration.renew().await?;
    assert!(registration.timestamp() > timestamp);
    assert_eq!(registration.revision(), 2);

    let _: i64 = redis
        .hset(&config_key, BTreeMap::from([("registration_data_max_field_value_bytes", "1")]))
        .await?;
    client.refresh_configuration().await?;
    data.insert("load".to_owned(), b"22".to_vec());
    let rejected = registration.update(&data).await;
    assert!(matches!(rejected, Err(error) if error.code() == Code::Capacity));
    data.insert("load".to_owned(), b"1".to_vec());
    let legacy_selector = FieldsSelector::new(
        &client,
        SelectorOptions {
            type_name: TYPE_NAME.to_owned(),
        },
    )
    .await?;
    assert!(
        legacy_selector
            .find(&uuid)
            .await?
            .is_some_and(|record| { record.meta.revision == 2 && record.data["state"] == b"ready" })
    );
    legacy_selector.close().await?;
    let _: i64 = redis
        .hset(&config_key, BTreeMap::from([("registration_data_max_field_value_bytes", "128")]))
        .await?;
    client.refresh_configuration().await?;

    redis.script_flush(false).await?;
    data.insert("load".to_owned(), b"2".to_vec());
    registration.update(&data).await?;

    let start_revision = registration.revision();
    let mut updates = JoinSet::new();
    for index in 0..24_u8 {
        let registration = Arc::clone(&registration);
        updates.spawn(async move {
            let data = fields([("load", [b'a' + index].as_slice()), ("state", b"ready".as_slice())]);
            registration.update(&data).await
        });
    }
    while let Some(result) = updates.join_next().await {
        result??;
    }
    assert!(registration.revision() > start_revision);
    assert!(registration.revision() <= start_revision + 24);
    let final_revision = registration.revision();
    wait_for_candidate(&selector, &uuid, Duration::from_secs(3), |record| record.meta.revision == final_revision).await?;

    let generation_before_repair = selector.snapshot().await?.generation;
    let event = encode_update_event(&uuid, final_revision + 2, registration.timestamp() + 1)?;
    let _: i64 = redis.publish(&registry_key, event).await?;
    tokio::time::sleep(Duration::from_millis(100)).await;
    let repaired = selector.snapshot().await?;
    assert!(repaired.synchronized);
    assert_eq!(repaired.generation, generation_before_repair);
    assert!(selector.find(&uuid).await?.is_some_and(|record| record.meta.revision == final_revision));

    register_raw(&redis, &zone, &registry_key, &raw_key).await?;
    wait_for_candidate(&selector, RAW_UUID, Duration::from_secs(1), |_| true).await?;
    wait_for_candidate_absence(&selector, RAW_UUID, Duration::from_secs(3)).await?;
    assert!(
        selector
            .find_retained(RAW_UUID)?
            .is_some_and(|retained| { retained.candidate.data["load"] == b"0" })
    );
    wait_for_retained_absence(&selector, RAW_UUID, Duration::from_secs(2)).await?;

    registration.close().await?;
    registration.close().await?;
    wait_for_candidate_absence(&selector, &uuid, Duration::from_secs(2)).await?;
    assert!(selector.find_retained(&uuid)?.is_none());
    selector.close().await?;
    selector.close().await?;
    client.close().await?;
    client.close().await?;
    transport.close().await?;
    let _: i64 = redis
        .del(vec![
            config_key,
            registry_key,
            raw_key,
            format!("verdandi:registration:{zone}:{TYPE_NAME}:{uuid}"),
        ])
        .await?;
    redis.quit().await?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn zone_configuration_refreshes_without_restart() -> Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let zone = unique_zone()?;
    let config_key = format!("verdandi:config:{zone}");
    let redis = direct_client(&endpoint).await?;
    let _: i64 = redis
        .hset(
            &config_key,
            BTreeMap::from([
                ("registration_attr_max_fields", "8"),
                ("registration_data_max_fields", "24"),
                ("configuration_refresh_ms", "1000"),
            ]),
        )
        .await?;
    let transport = Client::open(Config::new(&endpoint)).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        clock_refresh: Duration::from_secs(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let mut errors = client.subscribe_errors();
    let bootstrap = client.registration_limits();
    assert_eq!(bootstrap.attr_max_fields, 8);
    assert_eq!(bootstrap.data_max_fields, 24);
    assert_eq!(bootstrap.attr_value_max_bytes, 128);
    assert_eq!(bootstrap.data_value_max_bytes, 128);
    assert_eq!(bootstrap.configuration_refresh, Duration::from_secs(1));

    // A Selector owns RedisClock calibration but does not keep Registration
    // deployment policy fresh.
    let _: i64 = redis
        .hset(
            &config_key,
            BTreeMap::from([
                ("registration_attr_max_fields", "9"),
                ("registration_data_max_fields", "25"),
                ("configuration_refresh_ms", "1000"),
            ]),
        )
        .await?;
    let selector = FieldsSelector::new(
        &client,
        SelectorOptions {
            type_name: "configwatch".to_owned(),
        },
    )
    .await?;
    let before_time = command_calls(&redis, "cmdstat_time").await?;
    tokio::time::sleep(Duration::from_millis(1200)).await;
    let after_time = command_calls(&redis, "cmdstat_time").await?;
    assert!(after_time > before_time);
    let unchanged = client.registration_limits();
    assert_eq!(unchanged.attr_max_fields, 8);
    assert_eq!(unchanged.data_max_fields, 24);
    selector.close().await?;

    let registration = FieldsRegistration::new(
        &client,
        RegistrationOptions {
            type_name: "configwatch".to_owned(),
            ttl: Duration::from_secs(3),
            renew_interval: Some(Duration::from_secs(1)),
            version: 1,
        },
    )?;
    registration
        .register(&Fields::new(), &Fields::from([("load".to_owned(), b"0".to_vec())]))
        .await?;
    let loaded = client.registration_limits();
    assert_eq!(loaded.attr_max_fields, 9);
    assert_eq!(loaded.data_max_fields, 25);
    assert_eq!(loaded.configuration_refresh, Duration::from_secs(1));

    // Invalid administrative state is diagnosed without replacing the last
    // complete valid local snapshot.
    let _: i64 = redis.hset(&config_key, BTreeMap::from([("registration_attr_max_fields", "129")])).await?;
    let refresh_error = tokio::time::timeout(Duration::from_secs(2), errors.recv()).await??;
    assert_eq!(refresh_error.code(), Code::Capacity);
    let retained = client.registration_limits();
    assert_eq!(retained.attr_max_fields, 9);
    assert_eq!(retained.data_max_fields, 25);

    let _: i64 = redis
        .hset(
            &config_key,
            BTreeMap::from([("registration_attr_max_fields", "16"), ("registration_data_max_fields", "32")]),
        )
        .await?;
    wait_until(Duration::from_secs(2), || {
        let limits = client.registration_limits();
        limits.attr_max_fields == 16 && limits.data_max_fields == 32
    })
    .await?;

    registration.close().await?;
    let _: i64 = redis
        .hset(
            &config_key,
            BTreeMap::from([("registration_attr_max_fields", "7"), ("registration_data_max_fields", "23")]),
        )
        .await?;
    tokio::time::sleep(Duration::from_millis(1200)).await;
    let stopped = client.registration_limits();
    assert_eq!(stopped.attr_max_fields, 16);
    assert_eq!(stopped.data_max_fields, 32);
    client.refresh_configuration().await?;
    let explicit = client.registration_limits();
    assert_eq!(explicit.attr_max_fields, 7);
    assert_eq!(explicit.data_max_fields, 23);

    client.close().await?;
    transport.close().await?;
    let _: i64 = redis.del(config_key).await?;
    redis.quit().await?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn protocol_ceiling_registration_recovery() -> Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let zone = unique_zone()?;
    let config_key = format!("verdandi:config:{zone}");
    let redis = direct_client(&endpoint).await?;
    let _: i64 = redis
        .hset(
            &config_key,
            BTreeMap::from([
                ("registration_attr_max_fields", "128"),
                ("registration_data_max_fields", "128"),
                ("registration_attr_max_field_value_bytes", "256"),
                ("registration_data_max_field_value_bytes", "256"),
                ("registration_max_bytes", "65536"),
            ]),
        )
        .await?;
    let transport = Client::open(Config::new(&endpoint)).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_page_size: 32,
        selector_publish_interval: Duration::from_millis(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let selector = FieldsSelector::new(
        &client,
        SelectorOptions {
            type_name: "maximum".to_owned(),
        },
    )
    .await?;
    let attr: Fields = (0..128).map(|index| (format!("a{index:03}"), vec![b'a'; 240])).collect();
    let mut data: Fields = (0..128).map(|index| (format!("d{index:03}"), vec![b'd'; 240])).collect();
    let registration = FieldsRegistration::new(
        &client,
        RegistrationOptions {
            type_name: "maximum".to_owned(),
            ttl: Duration::from_secs(30),
            renew_interval: Some(Duration::from_secs(10)),
            version: 1,
        },
    )?;
    registration.register(&attr, &data).await?;
    let uuid = registration.uuid().to_owned();
    wait_for_candidate(&selector, &uuid, Duration::from_secs(5), |record| {
        record.meta.revision == 1 && record.attr.len() == 128 && record.data.len() == 128
    })
    .await?;
    let registration_key = format!("verdandi:registration:{zone}:maximum:{uuid}");
    let _: i64 = redis.del(&registration_key).await?;
    data.insert("d000".to_owned(), vec![b'x'; 240]);
    registration.update(&data).await?;
    wait_for_candidate(&selector, &uuid, Duration::from_secs(5), |record| {
        record.meta.revision == 2 && record.data.len() == 128 && record.data["d000"] == vec![b'x'; 240]
    })
    .await?;
    println!("protocol-ceiling complete Register recovery fields=256 value_bytes=240");

    registration.close().await?;
    selector.close().await?;
    client.close().await?;
    transport.close().await?;
    let _: i64 = redis
        .del(vec![config_key, format!("verdandi:registry:{zone}:maximum"), registration_key])
        .await?;
    redis.quit().await?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn registration_update_resets_automatic_renew() -> Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let zone = unique_zone()?;
    let redis = direct_client(&endpoint).await?;
    let transport = Client::open(Config::new(&endpoint)).await?;
    let client = RegistrationClient::open(&transport, RegistrationConfig::new(&zone)).await?;
    let registration = FieldsRegistration::new(
        &client,
        RegistrationOptions {
            type_name: "renewreset".to_owned(),
            ttl: Duration::from_secs(9),
            renew_interval: Some(Duration::from_secs(3)),
            version: 1,
        },
    )?;
    registration.register(&Fields::new(), &fields([("load", b"0".as_slice())])).await?;

    tokio::time::sleep(Duration::from_millis(1_800)).await;
    registration.update(&fields([("load", b"1".as_slice())])).await?;
    let updated_timestamp = registration.timestamp();
    tokio::time::sleep(Duration::from_secs(2)).await;
    if registration.timestamp() != updated_timestamp {
        return Err("automatic Renew used the pre-Update schedule".into());
    }
    wait_until(Duration::from_secs(4), || registration.timestamp() > updated_timestamp).await?;

    let uuid = registration.uuid().to_owned();
    registration.close().await?;
    client.close().await?;
    transport.close().await?;
    let _: i64 = redis
        .del(vec![
            format!("verdandi:config:{zone}"),
            format!("verdandi:registry:{zone}:renewreset"),
            format!("verdandi:registration:{zone}:renewreset:{uuid}"),
        ])
        .await?;
    redis.quit().await?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn dropping_last_registration_client_handle_stops_owned_workers() -> Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let zone = unique_zone()?;
    let redis = direct_client(&endpoint).await?;
    let transport = Client::open(Config::new(&endpoint)).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_publish_interval: Duration::from_millis(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let selector = FieldsSelector::new(
        &client,
        SelectorOptions {
            type_name: TYPE_NAME.to_owned(),
        },
    )
    .await?;
    let registration = FieldsRegistration::new(
        &client,
        RegistrationOptions {
            type_name: TYPE_NAME.to_owned(),
            ttl: Duration::from_secs(3),
            renew_interval: Some(Duration::from_millis(500)),
            version: 1,
        },
    )?;
    registration
        .register(&fields([("role", b"worker".as_slice())]), &fields([("load", b"0".as_slice())]))
        .await?;
    let uuid = registration.uuid().to_owned();
    wait_for_candidate(&selector, &uuid, Duration::from_secs(2), |_| true).await?;

    drop(client);
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        if selector.snapshot().await.is_err_and(|error| error.code() == Code::Unavailable) {
            break;
        }
        if Instant::now() >= deadline {
            return Err("Selector remained available after Client drop".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    assert!(selector.find(&uuid).await.is_err_and(|error| error.code() == Code::Unavailable));
    assert!(selector.find_retained(&uuid).is_err_and(|error| error.code() == Code::Unavailable));
    let update = registration.update(&fields([("load", b"1".as_slice())])).await;
    assert!(matches!(update, Err(error) if error.code() == Code::Closed));
    registration.close().await?;
    selector.close().await?;
    transport.close().await?;

    let _: i64 = redis
        .del(vec![
            format!("verdandi:config:{zone}"),
            format!("verdandi:registry:{zone}:{TYPE_NAME}"),
            format!("verdandi:registration:{zone}:{TYPE_NAME}:{uuid}"),
        ])
        .await?;
    redis.quit().await?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "requires Redis Sentinel in VERDANDI_SENTINEL_URL"]
async fn sentinel_registration_and_selector_reconcile() -> Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_SENTINEL_URL")?;
    let zone = unique_zone()?;
    let redis = direct_client(&endpoint).await?;
    let transport = Client::open(Config::new(&endpoint)).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_publish_interval: Duration::from_millis(1),
        clock_refresh: Duration::from_secs(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let selector = Selector::<Fields, Fields>::new(
        &client,
        SelectorOptions {
            type_name: "sentinel".to_owned(),
        },
    )
    .await?;
    let registration = Registration::<Fields, Fields>::new(
        &client,
        RegistrationOptions {
            type_name: "sentinel".to_owned(),
            ttl: Duration::from_secs(3),
            renew_interval: Some(Duration::from_millis(500)),
            version: 1,
        },
    )?;
    registration
        .register(&fields([("language", b"rust".as_slice())]), &fields([("value", b"before".as_slice())]))
        .await?;
    let uuid = registration.uuid().to_owned();
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if selector.find(&uuid).await?.is_some() {
            break;
        }
        if Instant::now() >= deadline {
            return Err("typed Sentinel Selector did not observe Registration".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    registration.update(&fields([("value", b"after".as_slice())])).await?;
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if selector
            .find(&uuid)
            .await?
            .is_some_and(|candidate| candidate.meta.revision == 2 && candidate.data["value"] == b"after")
        {
            break;
        }
        if Instant::now() >= deadline {
            return Err("typed Sentinel Selector did not reconcile Update".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }

    registration.close().await?;
    selector.close().await?;
    client.close().await?;
    transport.close().await?;
    let _: i64 = redis.del(format!("verdandi:config:{zone}")).await?;
    redis.quit().await?;
    Ok(())
}

async fn direct_client(endpoint: &str) -> Result<fred::clients::Client, fred::error::Error> {
    let config = fred::types::config::Config::from_url(endpoint)?;
    let client = Builder::from_config(config).build()?;
    client.init().await?;
    Ok(client)
}

async fn command_calls(redis: &fred::clients::Client, command: &str) -> Result<u64, Box<dyn StdError>> {
    let info: String = redis.info(Some(InfoKind::CommandStats)).await?;
    let prefix = format!("{command}:calls=");
    for line in info.lines() {
        let Some(fields) = line.trim().strip_prefix(&prefix) else {
            continue;
        };
        let calls = fields.split(',').next().ok_or("missing command call count")?.parse()?;
        return Ok(calls);
    }
    Err(format!("INFO commandstats omitted {command}").into())
}

async fn register_raw(redis: &fred::clients::Client, zone: &str, registry_key: &str, registration_key: &str) -> Result<(), fred::error::Error> {
    let script = Script::from_lua(include_str!("../src/registration/register.lua"));
    let arguments: Vec<Value> = vec![
        RAW_UUID.into(),
        "1".into(),
        "800".into(),
        "1".into(),
        ".role".into(),
        "transient".into(),
        "load".into(),
        "0".into(),
    ];
    let _: Value = script
        .evalsha_with_reload(
            redis,
            vec![registration_key.to_owned(), format!("verdandi:registry:{zone}:{TYPE_NAME}")],
            arguments,
        )
        .await?;
    assert_eq!(registry_key, format!("verdandi:registry:{zone}:{TYPE_NAME}"));
    Ok(())
}

fn fields<const N: usize>(values: [(&str, &[u8]); N]) -> Fields {
    values.into_iter().map(|(name, value)| (name.to_owned(), value.to_vec())).collect()
}

fn unique_zone() -> Result<String, String> {
    let mut random = [0_u8; 10];
    getrandom::fill(&mut random).map_err(|error| format!("random Zone: {error}"))?;
    let mut zone = String::from("Rust");
    for value in random {
        zone.push(char::from(b'a' + value % 26));
    }
    Ok(zone)
}

fn encode_update_event(uuid: &str, revision: u64, timestamp: u64) -> Result<Vec<u8>, rmpv::encode::Error> {
    let value = rmpv::Value::Array(vec![
        "&protocol".into(),
        "v1".into(),
        "&kind".into(),
        "update".into(),
        "@uuid".into(),
        uuid.into(),
        "@revision".into(),
        revision.into(),
        "@timestamp".into(),
        timestamp.into(),
        "load".into(),
        rmpv::Value::Binary(b"gap".to_vec()),
    ]);
    let mut output = Vec::new();
    rmpv::encode::write_value(&mut output, &value)?;
    Ok(output)
}

async fn wait_for_candidate<F>(selector: &FieldsSelector, uuid: &str, timeout: Duration, predicate: F) -> Result<Candidate<Fields, Fields>, Box<dyn StdError>>
where
    F: Fn(&Candidate<Fields, Fields>) -> bool,
{
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(candidate) = selector.find(uuid).await? {
            if predicate(&candidate) {
                return Ok(candidate);
            }
        }
        if Instant::now() >= deadline {
            return Err(format!("candidate {uuid} did not converge").into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

async fn wait_for_candidate_absence(selector: &FieldsSelector, uuid: &str, timeout: Duration) -> Result<(), Box<dyn StdError>> {
    let deadline = Instant::now() + timeout;
    loop {
        if selector.find(uuid).await?.is_none() {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err(format!("candidate {uuid} remained active").into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

async fn wait_for_retained_absence(selector: &FieldsSelector, uuid: &str, timeout: Duration) -> Result<(), Box<dyn StdError>> {
    let deadline = Instant::now() + timeout;
    loop {
        let retained: Option<RetainedCandidate<Fields, Fields>> = selector.find_retained(uuid)?;
        if retained.is_none() {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err(format!("candidate {uuid} remained retained").into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

async fn wait_until<F>(timeout: Duration, mut condition: F) -> Result<(), Box<dyn StdError>>
where
    F: FnMut() -> bool,
{
    let deadline = Instant::now() + timeout;
    while !condition() {
        if Instant::now() >= deadline {
            return Err(format!("condition did not converge before {:?}", SystemTime::now().duration_since(UNIX_EPOCH)?).into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    Ok(())
}
