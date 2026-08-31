use std::error::Error as StdError;
use std::time::{Duration, Instant};

use verdandi::registration::{Client as RegistrationClient, Config as RegistrationConfig};
use verdandi::{Client, Code, Config, Fields, HashValue};

#[derive(Debug, Default, Eq, PartialEq, HashValue)]
struct RootHashValue {
    #[redis(name = "revision")]
    revision: u64,
    #[redis(name = "name")]
    name: String,
    #[redis(name = "enabled")]
    enabled: bool,
}

#[tokio::test]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn root_commands_redis_integration() -> std::result::Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let client = Client::open(Config::new(endpoint)).await?;
    let mut random = [0_u8; 12];
    getrandom::fill(&mut random).map_err(|error| format!("random key: {error}"))?;
    let prefix = format!("verdandi:test:root:{}", hex::encode(random));
    let string_key = format!("{prefix}:string");
    let hash_key = format!("{prefix}:hash");

    client.ping().await?;
    assert_eq!(client.key().get::<u64>(&string_key).await?, None);
    client.key().store(&string_key, b"").await?;
    assert_eq!(client.key().load(&string_key).await?, Some(Vec::new()));
    client.key().set(&string_key, &42_u64).await?;
    assert_eq!(client.key().get::<u64>(&string_key).await?, Some(42));
    assert!(client.key().exists(&string_key).await?);
    assert!(matches!(client.hash().load(&string_key).await, Err(error) if error.code() == Code::Protocol));

    let mut fields = Fields::new();
    let _ = fields.insert("revision".to_owned(), b"7".to_vec());
    let _ = fields.insert("name".to_owned(), b"north".to_vec());
    client.hash().store(&hash_key, &fields).await?;
    assert_eq!(
        client.hash().get::<RootHashValue>(&hash_key).await?,
        RootHashValue {
            revision: 7,
            name: "north".to_owned(),
            enabled: false,
        }
    );
    client
        .hash()
        .set(
            &hash_key,
            &RootHashValue {
                revision: 8,
                name: "east".to_owned(),
                enabled: true,
            },
        )
        .await?;
    assert_eq!(client.hash().len(&hash_key).await?, 3);
    assert!(client.hash().contains_field(&hash_key, "enabled").await?);
    assert_eq!(client.hash().delete(&hash_key, &["enabled", "missing"]).await?, 1);

    client.key().with_ttl(Duration::from_millis(500)).set(&string_key, "expiring").await?;
    assert_eq!(client.key().get::<String>(&string_key).await?, Some("expiring".to_owned()));
    assert!(client.key().expire(&string_key, Duration::from_millis(100)).await?);
    let deadline = Instant::now() + Duration::from_secs(2);
    while client.key().exists(&string_key).await? {
        if Instant::now() >= deadline {
            return Err("expiring key remained present".into());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }

    assert!(client.key().delete(&hash_key).await?);
    assert!(!client.key().delete(&hash_key).await?);
    let _ = client.key().delete(&string_key).await;
    client.close().await?;
    Ok(())
}

#[tokio::test]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn one_transport_supports_independent_registration_zones() -> std::result::Result<(), Box<dyn StdError>> {
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let first_zone = random_zone()?;
    let second_zone = random_zone()?;
    let transport = Client::open(Config::new(endpoint)).await?;
    let first = RegistrationClient::open(&transport, RegistrationConfig::new(&first_zone)).await?;
    let second = RegistrationClient::open(&transport, RegistrationConfig::new(&second_zone)).await?;

    let first_key = format!("verdandi:config:{first_zone}");
    let second_key = format!("verdandi:config:{second_zone}");
    assert!(transport.key().exists(&first_key).await?);
    assert!(transport.key().exists(&second_key).await?);
    assert!(transport.key().delete(&first_key).await?);
    assert!(transport.key().delete(&second_key).await?);

    first.close().await?;
    transport.close().await?;
    assert!(matches!(transport.ping().await, Err(error) if error.code() == Code::Closed));
    assert!(matches!(
        RegistrationClient::open(&transport, RegistrationConfig::new(random_zone()?)).await,
        Err(error) if error.code() == Code::Closed
    ));
    second.close().await?;
    Ok(())
}

fn random_zone() -> std::result::Result<String, Box<dyn StdError>> {
    let mut random = [0_u8; 12];
    getrandom::fill(&mut random).map_err(|error| format!("random Zone: {error}"))?;
    Ok(random.into_iter().map(|byte| char::from(b'a' + byte % 26)).collect())
}
