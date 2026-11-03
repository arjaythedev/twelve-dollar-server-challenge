class HealthController < ApplicationController
  allow_unauthenticated_access

  def show
    ActiveRecord::Base.connection.select_value("SELECT 1")
    render json: { status: "ok", db: "ok", uptime_s: uptime_seconds }
  rescue StandardError => error
    render json: { status: "degraded", db: "unreachable", error: error.message }, status: :service_unavailable
  end

  private

  def uptime_seconds
    (Process.clock_gettime(Process::CLOCK_MONOTONIC) - Rails.configuration.x.booted_at).to_i
  end
end
