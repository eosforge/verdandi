use super::Options;

#[test]
fn valid_cli_uses_bounded_defaults_and_accepts_equals() {
    let parsed = Options::parse(["--cluster=alpha", "--super=127.0.0.1:7442", "--listen=127.0.0.1:0"].map(str::to_owned));
    assert!(parsed.is_ok_and(|options| options.is_some_and(|options| options.workers == 2 && options.shutdown_timeout.as_secs() == 5)));
}

#[test]
fn cli_rejects_invalid_or_conflicting_configuration() {
    for extra in [
        "--cluster=other",
        "--worker-threads=0",
        "--worker-threads=65",
        "--shutdown-timeout-seconds=61",
        "--id=test",
        "--seed=",
        "unused",
    ] {
        let args = ["--cluster=alpha", "--super=127.0.0.1:7442", "--listen=127.0.0.1:0", extra].map(str::to_owned);
        assert!(Options::parse(args).is_err(), "accepted {extra}");
    }
    assert!(Options::parse(["--help"].map(str::to_owned)).is_ok_and(|value| value.is_none()));
}
