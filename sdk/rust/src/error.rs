use std::error::Error as StdError;
use std::fmt::{Display, Formatter};

/// 跨语言稳定、可供程序判断的 Verdandi 结果类别。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[non_exhaustive]
pub enum Code {
    Invalid,
    Protocol,
    Contract,
    Target,
    Capacity,
    Missing,
    Stale,
    Transition,
    Immutable,
    Corrupt,
    Unavailable,
    Deadline,
    Ambiguous,
    Closed,
}

impl Code {
    /// 把协议中的稳定状态字符串转换为 `Code`。
    ///
    /// `value` 必须精确匹配已知小写名称；未知值返回 `None`，供协议解析器判定损坏。
    pub(crate) fn from_status(value: &str) -> Option<Self> {
        Some(match value {
            "invalid" => Self::Invalid,
            "protocol" => Self::Protocol,
            "contract" => Self::Contract,
            "target" => Self::Target,
            "capacity" => Self::Capacity,
            "missing" => Self::Missing,
            "stale" => Self::Stale,
            "transition" => Self::Transition,
            "immutable" => Self::Immutable,
            "corrupt" => Self::Corrupt,
            "unavailable" => Self::Unavailable,
            "deadline" => Self::Deadline,
            "ambiguous" => Self::Ambiguous,
            "closed" => Self::Closed,
            _ => return None,
        })
    }

    /// 返回当前类别的稳定小写协议名称。
    pub(crate) const fn as_str(self) -> &'static str {
        match self {
            Self::Invalid => "invalid",
            Self::Protocol => "protocol",
            Self::Contract => "contract",
            Self::Target => "target",
            Self::Capacity => "capacity",
            Self::Missing => "missing",
            Self::Stale => "stale",
            Self::Transition => "transition",
            Self::Immutable => "immutable",
            Self::Corrupt => "corrupt",
            Self::Unavailable => "unavailable",
            Self::Deadline => "deadline",
            Self::Ambiguous => "ambiguous",
            Self::Closed => "closed",
        }
    }
}

/// 一个带有界可选上下文的稳定 Verdandi 错误。
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Error {
    code: Code,
    field: Option<String>,
    revision: Option<u64>,
    detail: Option<String>,
}

impl Error {
    /// 只用机器可读 `code` 构造错误，其余上下文为空。
    pub const fn new(code: Code) -> Self {
        Self {
            code,
            field: None,
            revision: None,
            detail: None,
        }
    }

    /// 用 `code` 和拒绝或损坏的协议 `field` 构造错误。
    pub fn field(code: Code, field: impl Into<String>) -> Self {
        Self {
            code,
            field: Some(field.into()),
            revision: None,
            detail: None,
        }
    }

    /// 把底层驱动或运行时 `error` 限长包装为 `code`。
    ///
    /// 诊断文本最多保留 512 字节，避免外部错误无限放大日志或返回值。
    pub(crate) fn driver(code: Code, error: impl Display) -> Self {
        let mut detail = error.to_string();
        detail.truncate(512);
        Self {
            code,
            field: None,
            revision: None,
            detail: Some(detail),
        }
    }

    /// 同时记录稳定 `code`、协议 `field` 和限长底层 `error`。
    pub(crate) fn field_driver(code: Code, field: impl Into<String>, error: impl Display) -> Self {
        let mut detail = error.to_string();
        detail.truncate(512);
        Self {
            code,
            field: Some(field.into()),
            revision: None,
            detail: Some(detail),
        }
    }

    /// 附加本次拒绝关联的权威 `revision` 并返回错误自身，便于链式构造。
    pub(crate) fn with_revision(mut self, revision: u64) -> Self {
        self.revision = Some(revision);
        self
    }

    /// 仅在错误尚无字段时补入 `field`，保留更内层、更精确的定位。
    pub(crate) fn with_field_if_empty(mut self, field: &str) -> Self {
        if self.field.is_none() {
            self.field = Some(field.to_owned());
        }
        self
    }

    /// 返回机器可读的稳定结果类别。
    pub const fn code(&self) -> Code {
        self.code
    }

    /// 返回被拒绝或损坏的协议字段；不适用时为 `None`。
    pub fn field_name(&self) -> Option<&str> {
        self.field.as_deref()
    }

    /// 返回拒绝结果附带的权威 revision；不适用时为 `None`。
    pub const fn revision(&self) -> Option<u64> {
        self.revision
    }
}

impl Display for Error {
    /// 生成有界、供日志诊断使用的文本表示。
    ///
    /// `formatter` 接收稳定类别及存在的字段、revision 和底层详情；机器判断应使用 `code()`。
    fn fmt(&self, formatter: &mut Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "verdandi: {}", self.code.as_str())?;
        if let Some(field) = &self.field {
            write!(formatter, ": field {field}")?;
        }
        if let Some(revision) = self.revision {
            write!(formatter, ": revision {revision}")?;
        }
        if let Some(detail) = &self.detail {
            write!(formatter, ": {detail}")?;
        }
        Ok(())
    }
}

impl StdError for Error {}

pub type Result<T> = std::result::Result<T, Error>;
