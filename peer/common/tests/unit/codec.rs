//! 验证字节预算内的 repeated 放大与畸形字段都在对象构建前被拒绝.
use super::*;
#[test]
fn repeated_empty_members_are_counted_before_decode() {
    let data = [10_u8, 0].repeat(4097);
    assert!(check_members(&data[..8192], 4096).is_ok());
    assert_eq!(
        check_members(data.as_slice(), 4096).err().map(|e| e.code()),
        Some(tonic::Code::ResourceExhausted)
    );
    assert!(check_members(&[10, 128][..], 4096).is_err());
    assert!(check_members(&[8, 0][..], 4096).is_err());
    assert!(check_members(&[18, 0, 10, 0][..], 1).is_ok());
}

#[test]
fn unknown_fields_do_not_hide_top_level_members_or_break_forward_compatibility() {
    // 未知 group 内的同号字段不属于顶层成员列表; 未知字段仍按 Protobuf 规则跳过.
    assert!(check_members(&[27, 10, 0, 28, 10, 0][..], 1).is_ok());
    for unknown in [vec![32, 1], vec![37, 0, 0, 0, 0], vec![34, 2, 8, 1]] {
        let mut payload = vec![10, 0];
        payload.extend(unknown);
        payload.extend([10, 0]);
        assert_eq!(
            check_members(payload.as_slice(), 1).err().map(|e| e.code()),
            Some(tonic::Code::ResourceExhausted)
        );
        assert!(check_members(payload.as_slice(), 2).is_ok());
    }
}

#[test]
fn malformed_lengths_varints_and_deep_groups_are_bounded() {
    for data in [vec![0], vec![255; 11], vec![10, 255, 255, 255, 255, 15], vec![14], vec![27; 150], vec![28]] {
        assert!(check_members(data.as_slice(), 4096).is_err());
    }
    // 有限确定性字节语料补充畸形 key/长度组合. 不引入随机依赖或声称这是覆盖引导 fuzz.
    let mut random = 0x714ab912_u32;
    for length in 0..128 {
        let data: Vec<_> = (0..length)
            .map(|_| {
                random = random.wrapping_mul(1664525).wrapping_add(1013904223);
                (random >> 24) as u8
            })
            .collect();
        let result = check_members(data.as_slice(), 4096);
        if RegistrationResponse::decode(data.as_slice()).is_ok() {
            assert!(result.is_ok());
        }
    }
}
