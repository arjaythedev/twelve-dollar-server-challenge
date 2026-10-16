package domain

// Health is what GET /health answers with. Healthy, it carries the uptime;
// degraded, the error that made it so.
//
//ggen:generate nosortkeys
type Health struct {
	Status  string `json:"status"`
	DB      string `json:"db"`
	UptimeS *int64 `json:"uptime_s,omitempty"`
	Error   string `json:"error,omitempty"`
}

// ErrorBody is every error response.
//
//ggen:generate
type ErrorBody struct {
	Error string `json:"error"`
}
