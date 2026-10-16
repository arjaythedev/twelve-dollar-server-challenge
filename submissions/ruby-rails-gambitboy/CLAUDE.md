# Project Conventions

## General

- No service classes (`app/services/`) or lib classes (`lib/`)
- For shared logic, use concerns or POROs in `app/models/`
- Prefer built-in Rails features over custom code

## No Guessing

Do not guess column names, table names, association names, method names, or any other data shape. Always verify by reading the source — `../../schema.sql`, the relevant model, or grep — before suggesting a console command, SQL, or code change. If you can't verify it, say so instead of assuming.

## Routes

**No custom controller actions.** Always use nested controllers instead.

**No `only: []` on resources.** If a resource has no standard actions, omit the option entirely.

Bad:
```ruby
resources :posts do
  member do
    get :archive
  end
end
```

Good:
```ruby
resources :posts do
  resource :archive
end
```

## Controllers

When chaining multiple method calls, put each call on its own line with the dots aligned under the receiver.

Bad:
```ruby
users = User.where(active: true).by_search(params[:search]).order(:last_name)
```

Good:
```ruby
users = User.where(active: true)
            .by_search(params[:search])
            .order(:last_name)
```

**Denest with guard clauses.** Handle the success (or happy) path with an early `return render …` and let the failure path fall through — don't wrap the action body in `if/else`.

Bad:
```ruby
def update
  if current_user.update(user_params)
    render json: UserResource.new(current_user).serialize
  else
    render json: { errors: current_user.errors }, status: :unprocessable_entity
  end
end
```

Good:
```ruby
def update
  return render json: UserResource.new(current_user).serialize if current_user.update(user_params)

  render json: { errors: current_user.errors }, status: :unprocessable_entity
end
```

## Models

- Use `belongs_to` with `optional: true` for optional associations
- Extract related functionality into concerns under `app/models/concerns/model_name/`

Example:
```ruby
# app/models/concerns/user/manageable.rb
module User::Manageable
  extend ActiveSupport::Concern

  included do
    belongs_to :manager, class_name: "User", optional: true
  end
end

# app/models/user.rb
class User < ApplicationRecord
  include Manageable
end
```

## Code comments

Refrain from adding comments when writing or editing code. We have a no comments policy. If you require comments to explain your code the code is bad and should be rethought and improved or refactored.
