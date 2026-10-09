package httpapi

import (
	"context"
	"net/http"
	"time"

	"feed/domain"
	"feed/internal/repository"
)

type HealthApi struct {
	DB      repository.Pinger
	Started time.Time
}

//rr:route GET /health
func (h *HealthApi) Health(ctx context.Context, w http.ResponseWriter) domain.Health {
	if err := h.DB.Ping(ctx); err != nil {
		w.Header()["Content-Type"] = jsonCT
		w.WriteHeader(http.StatusServiceUnavailable)
		return domain.Health{Status: "degraded", DB: "unreachable", Error: err.Error()}
	}
	up := int64(time.Since(h.Started) / time.Second)
	return domain.Health{Status: "ok", DB: "ok", UptimeS: &up}
}
