use super::*;

#[test]
fn diagnostics_preserve_utf8_and_stable_error_context() {
    let inputs = [
        String::new(),
        "x".repeat(511),
        "x".repeat(512),
        "x".repeat(513),
        "中".repeat(200),
        format!("x{}", "🙂".repeat(128)),
    ];
    for input in inputs {
        for error in [
            Error::driver(Code::Unavailable, &input),
            Error::field_driver(Code::Unavailable, "payload", &input),
        ] {
            let Some(detail) = error.detail.as_deref() else {
                panic!("diagnostic was lost");
            };
            assert!(detail.len() <= 512);
            assert!(input.starts_with(detail));
            assert!(input.len() <= 512 || detail.len() >= 509);
            assert_eq!(error.code(), Code::Unavailable);
        }
    }
}
