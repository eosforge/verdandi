use super::*;
use crate::test_support::*;

#[test]
fn candidates_reject_role_cluster_order_and_duplicate_identity() -> io::Result<()> {
    let identity = Identity::load(&fixture("peer-a"))?;
    let process = ProcessIdentity::new()?;
    let first = member(&identity, &process, "127.0.0.1:7001".parse().map_err(io::Error::other)?, 1);
    let mut second = member(
        &Identity::load(&fixture("peer-b"))?,
        &ProcessIdentity::new()?,
        "127.0.0.1:7002".parse().map_err(io::Error::other)?,
        1,
    );
    second.group = "remote".into();
    let valid = [first, second];
    let check = |values: &[Member]| validate_candidates(values, "alpha", "default", "planet");
    assert!(check(&[]).is_ok());
    assert!(check(&valid).is_ok());
    let mut reversed = valid.clone();
    reversed.reverse();
    assert!(check(&reversed).is_err());
    for case in 0..6 {
        let mut invalid = valid.clone();
        match case {
            0 => invalid[1].role = NodeRole::Planet as i32,
            1 => invalid[1].cluster_id = "other".into(),
            2 => invalid[1].principal = invalid[0].principal.clone(),
            3 => invalid[1].advertise = invalid[0].advertise.clone(),
            4 => invalid[1].peer_id = invalid[0].peer_id.clone(),
            _ => invalid[1].role = NodeRole::Unspecified as i32,
        }
        assert!(check(&invalid).is_err(), "case {case}");
    }
    assert!(validate_candidates(&valid, "alpha", "default", &valid[0].principal).is_err());
    Ok(())
}

#[test]
fn planet_candidate_object_budget_is_eight() -> io::Result<()> {
    let identity = Identity::load(&fixture("peer-a"))?;
    let first = member(&identity, &ProcessIdentity::new()?, "127.0.0.1:7001".parse().map_err(io::Error::other)?, 1);
    assert!(validate_candidates(&vec![first; 9], "alpha", "default", "planet").is_err());
    Ok(())
}
