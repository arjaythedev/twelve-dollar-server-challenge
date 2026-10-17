## Shared data model for the submission.

type Post* = object
  id*: int64
  body*: string        ## already trimmed (as stored / wire text)
  createdAt*: string   ## "YYYY-MM-DDTHH:MM:SS.mmmZ"
  author*: string
  likeCount*: int64
