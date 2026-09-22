package web

import (
	"crypto/rand"
	"encoding/base64"
	"net/http"
	"sync"
	"time"
)

// sessions 保存当前进程的固定八小时登录, 值中的 time.Time 含本地单调分量, 不滑动续期.
type sessions struct {
	mutex   sync.Mutex
	values  map[string]time.Time
	maximum int
}

// create 在 KDF 成功后原子替换本浏览器的 previous, 满容量时仍可重新登录, 不提前撤销旧令牌.
// 随机源在锁外调用, 只在安装时检查容量/碰撞, 不让普通会话查询等待随机源.
func (sessions *sessions) create(now time.Time, previous string) (string, time.Time, bool) {
	for attempt := 0; attempt < 4; attempt++ {
		var bytes [32]byte
		if _, err := rand.Read(bytes[:]); err != nil {
			return "", time.Time{}, false
		}
		token := base64.RawURLEncoding.EncodeToString(bytes[:])
		sessions.mutex.Lock()
		for key, expire := range sessions.values {
			if !now.Before(expire) {
				delete(sessions.values, key)
			}
		}
		_, replacing := sessions.values[previous]
		if len(sessions.values) >= sessions.maximum && !replacing {
			sessions.mutex.Unlock()
			return "", time.Time{}, false
		}
		if _, exists := sessions.values[token]; !exists {
			expire := now.Add(8 * time.Hour)
			sessions.values[token] = expire
			delete(sessions.values, previous)
			sessions.mutex.Unlock()
			return token, expire, true
		}
		sessions.mutex.Unlock()
	}
	return "", time.Time{}, false
}

// get 每次接纳验证真实期限, 过期记录只删除当前一项, 不为普通管理请求扫描全表.
func (sessions *sessions) get(token string, now time.Time) (time.Time, bool) {
	sessions.mutex.Lock()
	defer sessions.mutex.Unlock()
	expire, exists := sessions.values[token]
	if !exists || !now.Before(expire) {
		delete(sessions.values, token)
		return time.Time{}, false
	}
	return expire, true
}

// remove 在返回清除 Cookie 之前撤销服务端会话, 缺项和重复注销都不产生错误.
func (sessions *sessions) remove(token string) {
	sessions.mutex.Lock()
	delete(sessions.values, token)
	sessions.mutex.Unlock()
}

// token 只接受唯一 Cookie, 不从 URL、Authorization 或 localStorage 替代通道恢复会话.
func token(request *http.Request) string {
	cookies := request.CookiesNamed("astra-session")
	if len(cookies) != 1 || len(cookies[0].Value) != 43 {
		return ""
	}
	return cookies[0].Value
}

// cookie 的创建/删除共用全部作用域属性, Domain 始终为空, 固定期限不因查询而延长.
func (server *Server) cookie(response http.ResponseWriter, value string, expire time.Time) {
	age := int(time.Until(expire) / time.Second)
	if value == "" || age <= 0 {
		value, age, expire = "", -1, time.Unix(1, 0)
	}
	mode := http.SameSiteLaxMode
	if server.crosssite {
		mode = http.SameSiteNoneMode
	}
	http.SetCookie(response, &http.Cookie{Name: "astra-session", Value: value, Path: "/api", Expires: expire, MaxAge: age, HttpOnly: true, Secure: server.secure, SameSite: mode})
}
