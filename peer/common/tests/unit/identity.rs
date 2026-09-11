//! UUID, 签名 bearer 凭证与成员语义边界.
use super::*;
use crate::test_support::{fixture, member, signed};
use std::{collections::BTreeSet, net::SocketAddr};

#[test]
fn process_identity_is_fresh_and_canonical() -> io::Result<()> {
    let mut ids = BTreeSet::new();
    for _ in 0..128 {
        let process = ProcessIdentity::new()?;
        assert!(valid_uuid(&process.id));
        assert!(ids.insert(process.id));
    }
    Ok(())
}
#[test]
fn bearer_signature_is_reusable_but_tampering_fails() -> io::Result<()> {
    let identity = Identity::load(&fixture("peer-a"))?;
    let process = ProcessIdentity::new()?;
    let member = member(&identity, &process, SocketAddr::from(([127, 0, 0, 1], 7001)), 1);
    let hello = signed(&member)?;
    assert_eq!(identity.verify(&hello).map_err(SessionError::into_io)?, member);
    assert!(Identity::load(&fixture("peer-b"))?.verify(&hello).is_ok());
    let mut changed = hello.clone();
    changed.admission = Bytes::from_static(b"tampered");
    assert!(identity.verify(&changed).is_err());
    changed = hello;
    changed.admission_signature = Bytes::from(vec![0; 64]);
    assert!(identity.verify(&changed).is_err());
    // 账号相同不等于节点相同, 指纹按规范端点区分.
    assert_ne!(
        identity.endpoint_principal("alpha", "127.0.0.1:7001"),
        identity.endpoint_principal("alpha", "127.0.0.1:7002")
    );
    Ok(())
}
#[test]
fn malformed_members_fail_even_with_authority_signature() -> io::Result<()> {
    let identity = Identity::load(&fixture("peer-a"))?;
    let process = ProcessIdentity::new()?;
    let valid = member(&identity, &process, SocketAddr::from(([127, 0, 0, 1], 7001)), 1);
    for address in [
        "0.0.0.0:1",
        "127.0.0.1:0",
        "224.0.0.1:1",
        "[::ffff:127.0.0.1]:1",
        "[fe80::1%2]:1",
        "[0:0:0:0:0:0:0:1]:1",
        "localhost:1",
    ] {
        let mut bad = valid.clone();
        bad.advertise = address.into();
        assert!(validate_member(&bad).is_err());
        assert!(identity.verify(&signed(&bad)?).is_err());
    }
    for field in 0..5 {
        let mut bad = valid.clone();
        match field {
            0 => bad.cluster_id = "x/../../y".into(),
            1 => bad.peer_id = "0".repeat(32),
            2 => bad.principal = "F".repeat(64),
            3 => bad.epoch = 0,
            _ => bad.role = 999,
        }
        assert!(validate_member(&bad).is_err());
        assert!(identity.verify(&signed(&bad)?).is_err());
    }
    Ok(())
}

#[test]
fn shared_rust_go_admission_vectors() -> io::Result<()> {
    let vectors: serde_json::Value = serde_json::from_str(include_str!("../../../tests/fixtures/admission-v4.json"))?;
    for group in ["names", "addresses", "uuids"] {
        let cases = vectors[group].as_array().ok_or_else(|| failure("missing shared vectors"))?;
        assert!(!cases.is_empty());
        for case in cases {
            let value = case["value"].as_str().ok_or_else(|| failure("missing vector value"))?;
            let accepted = match group {
                "names" => validate_name("name", value).is_ok(),
                "uuids" => valid_uuid(value),
                _ => value
                    .parse::<SocketAddr>()
                    .is_ok_and(|address| validate_remote_address("member", address).is_ok() && address.to_string() == value),
            };
            assert_eq!(Some(accepted), case["valid"].as_bool(), "{group}: {value}");
        }
    }
    let mut identity = Identity::load(&fixture("peer-a"))?;
    for case in vectors["principals"].as_array().ok_or_else(|| failure("missing principal vectors"))? {
        let field = |key: &str| case[key].as_str().ok_or_else(|| failure("missing vector field"));
        identity.username = field("username")?.into();
        assert_eq!(identity.endpoint_principal(field("cluster")?, field("address")?), field("sha256")?);
    }
    Ok(())
}

#[test]
fn own_certificate_is_checked_for_expiry_trust_and_endpoint() -> io::Result<()> {
    let loopback = SocketAddr::from(([127, 0, 0, 1], 7443));
    assert!(Identity::load(&fixture("peer-a"))?.validate_endpoint(loopback).is_ok());
    assert!(Identity::load(&fixture("expired"))?.validate_endpoint(loopback).is_err());
    assert!(
        Identity::load(&fixture("peer-a"))?
            .validate_endpoint(SocketAddr::from(([192, 0, 2, 1], 7443)))
            .is_err()
    );
    Ok(())
}
