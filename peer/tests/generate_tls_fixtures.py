"""显式更新公开测试 TLS 证书为 ECDSA P-256, 不生成或修改任何部署身份."""

from datetime import datetime, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def main():
    """保留每个夹具的主题、有效期、SAN 和用途, 准入签名密钥及账号文件保持原样."""
    root = Path(__file__).resolve().parent / "fixtures"
    names = ("supervisor", "peer-a", "peer-b", "peer-c", "peer-d", "planet-a", "planet-b", "wrong-cluster", "expired", "rogue")
    # 先读齐模板再写入. CA 私钥只存在本次进程内, 不保存到仓库或系统证书库.
    templates = {name: x509.load_pem_x509_certificate((root / name / "cert.pem").read_bytes()) for name in names}
    authority = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Verdandi PUBLIC TEST ECDSA CA")])
    ca = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(authority.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime(2000, 1, 1, tzinfo=timezone.utc))
        .not_valid_after(datetime(2100, 1, 1, tzinfo=timezone.utc))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
        .sign(authority, hashes.SHA256())
    )
    for name, previous in templates.items():
        key = ec.generate_private_key(ec.SECP256R1())
        builder = (
            x509.CertificateBuilder()
            .subject_name(previous.subject)
            .issuer_name(subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(previous.not_valid_before_utc)
            .not_valid_after(previous.not_valid_after_utc)
        )
        for extension in previous.extensions:
            if isinstance(extension.value, (x509.SubjectKeyIdentifier, x509.AuthorityKeyIdentifier)):
                continue
            builder = builder.add_extension(extension.value, extension.critical)
        # rogue 的颁发者名字相同但签名密钥错误, 保留原来的不可信证书测试语义.
        signer = ec.generate_private_key(ec.SECP256R1()) if name == "rogue" else authority
        certificate = builder.sign(signer, hashes.SHA256())
        (root / name / "ca.pem").write_bytes(ca.public_bytes(serialization.Encoding.PEM))
        (root / name / "cert.pem").write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        (root / name / "key.pem").write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    print("Updated public test TLS fixtures; admission keys and login files unchanged")


if __name__ == "__main__":
    main()
