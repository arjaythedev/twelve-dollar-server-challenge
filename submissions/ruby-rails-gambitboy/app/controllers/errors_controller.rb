class ErrorsController < ApplicationController
  allow_unauthenticated_access

  def not_found
    render_error("not found", :not_found)
  end
end
