//! gRPC 状态必须保留暂时故障与永久拒绝的区别, 且不回显远端材料.
use super::*;
use tonic::{Code, Status};

#[test]
fn rpc_status_classification_preserves_retry_and_redacts_details() {
    use io::ErrorKind::*;
    for (code, expected) in [
        (Code::Cancelled, Interrupted),
        (Code::Unknown, ConnectionAborted),
        (Code::Unavailable, ConnectionAborted),
        (Code::DeadlineExceeded, TimedOut),
        (Code::ResourceExhausted, WouldBlock),
        (Code::Unauthenticated, PermissionDenied),
        (Code::PermissionDenied, PermissionDenied),
        (Code::AlreadyExists, AlreadyExists),
        (Code::Aborted, AlreadyExists),
        (Code::InvalidArgument, InvalidData),
        (Code::OutOfRange, InvalidData),
        (Code::FailedPrecondition, InvalidData),
        (Code::Unimplemented, InvalidData),
        (Code::Internal, InvalidData),
        (Code::DataLoss, InvalidData),
    ] {
        let error = rpc_error(Status::with_details(code, "secret-password", b"private-details".as_slice().into()));
        assert_eq!(error.kind(), expected, "{code:?}");
        assert!(!format!("{error:?}").contains("secret"));
        assert!(!format!("{error:?}").contains("private"));
    }
}
