class ApplicationRecord < ActiveRecord::Base
  primary_abstract_class

  self.record_timestamps = false
end
