Rails.application.routes.draw do
  get "health" => "health#show"

  resource :feed, only: %i[show]

  resources :posts, only: %i[show create], format: false, constraints: { id: %r{[^/]+}, post_id: %r{[^/]+} } do
    scope module: :posts do
      resource :like, only: %i[create]
    end
  end

  match "/", to: "errors#not_found", via: :all
  match "*path", to: "errors#not_found", via: :all, format: false
end
