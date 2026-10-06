module application
  use bindings
  use unicode
  use, intrinsic :: ieee_arithmetic
  implicit none
  type(c_ptr) :: db,feed,post,create,like,exists,health,valid,body_query,auth_query
  character(:), allocatable :: secret
  real(c_double) :: started
  logical :: transaction=.false., failed=.false.
contains
  function prepare(sql) result(s)
    character(*), intent(in) :: sql
    type(c_ptr) :: s
    s=db_prepare(db,sql//c_null_char)
  end function
  logical function strict_json(s) result(ok)
    character(*), intent(in) :: s
    integer :: rc
    ok=.false.
    if(len(s)==0.or.index(s,c_null_char)/=0) return
    if(.not.valid_utf8(s)) return
    call db_text(valid,1,s,len(s))
    rc=sqlite3_step(valid)
    if(rc==100) ok=sqlite3_column_int64(valid,0)/=0
    call db_reset(valid)
  end function
  subroutine authenticate(auth,user,name,message)
    character(*), intent(in) :: auth
    integer(c_int64_t), intent(out) :: user
    character(:), allocatable, intent(out) :: name,message
    character(:), allocatable :: token,head,payload,buffer
    integer :: i,c,first,second,n,rc
    real(c_double) :: expiry,nbf,now
    user=0; name=''; message='missing bearer token'
    if(len(auth)<7) return
    if(auth(:7)/='Bearer ') return
    message='invalid or expired token'
    token=auth(8:); first=0; second=0
    do i=1,len(token)
      c=iachar(token(i:i))
      if(c==46) then
        if(first==0) then
          first=i
        else if(second==0) then
          second=i
        else
          return
        end if
      else if(.not.((c>=65.and.c<=90).or.(c>=97.and.c<=122).or. &
          (c>=48.and.c<=57).or.c==45.or.c==95)) then
        return
      end if
    end do
    if(first<=1.or.second<=first+1.or.len(token)-second/=43) return
    if(crypto_verify(secret,len(secret),token(:second-1),second-1,token(second+1:),43)==0) return
    allocate(character(len(token)+4) :: buffer)
    n=base64_decode(token(:first-1),first-1,buffer)
    if(n<1) return
    head=buffer(:n)
    n=base64_decode(token(first+1:second-1),second-first-1,buffer)
    if(n<1) return
    payload=buffer(:n)
    if(.not.strict_json(head)) return
    if(.not.strict_json(payload)) return
    call db_text(auth_query,1,head,len(head))
    call db_text(auth_query,2,payload,len(payload))
    rc=sqlite3_step(auth_query)
    if(rc==100) then
      now=wall_time()
      expiry=sqlite3_column_double(auth_query,0)
      nbf=sqlite3_column_double(auth_query,1)
      if(sqlite3_column_int64(auth_query,2)/=0.and.ieee_is_finite(expiry).and.ieee_is_finite(nbf) &
          .and.expiry>now.and.nbf<=now) then
        message='invalid token payload'
        user=positive(column(auth_query,4))
        name=column(auth_query,5)
        if(sqlite3_column_int64(auth_query,3)/=0.and.user>0.and.valid_utf8(name)) message=''
      end if
    end if
    call db_reset(auth_query)
  end subroutine
  function error_body(message) result(out)
    character(*), intent(in) :: message
    character(:), allocatable :: out
    out='{"error":'//quote(message)//'}'
  end function
  function row(s) result(out)
    type(c_ptr), value :: s
    character(:), allocatable :: out
    out='{"id":'//number(sqlite3_column_int64(s,0))//',"body":'//quote(column(s,1))// &
      ',"created_at":'//quote(column(s,2))//',"author":'//quote(column(s,3))// &
      ',"like_count":'//number(sqlite3_column_int64(s,4))//'}'
  end function
  logical function begin_write() result(ok)
    integer :: rc
    ok=.not.failed
    if(.not.ok.or.transaction) return
    rc=db_exec(db,'BEGIN IMMEDIATE'//c_null_char)
    ok=rc==0
    transaction=ok
  end function
  subroutine dispatch(method,path,auth,input,status,out)
    character(*), intent(in) :: method,path,auth,input
    integer(c_int), intent(out) :: status
    character(:), allocatable, intent(out) :: out
    character(:), allocatable :: id_text,name,message,body,created,id_string
    integer(c_int64_t) :: id,user,new_id
    integer :: rc,count,changed
    logical :: get,submit,create_route,like_route,feed_route
    type(c_ptr) :: statement
    status=500; out=error_body('internal server error')
    if(failed) return
    get=method=='GET'; submit=method=='POST'
    if(get.and.path=='/health') then
      rc=sqlite3_step(health)
      call db_reset(health)
      status=200
      out='{"status":"ok","db":"ok","uptime_s":'// &
        number(int(monotonic_time()-started,c_int64_t))//'}'
      if(rc/=100) then
        status=503
        out='{"status":"degraded","db":"unreachable","error":'//quote(cstring(sqlite3_errmsg(db)))//'}'
      end if
      return
    end if
    create_route=submit.and.path=='/posts'
    feed_route=get.and.path=='/feed'
    like_route=.false.; id_text=''
    if(len(path)>12.and.submit) like_route=path(len(path)-4:)=='/like'
    if(len(path)>7) then
      if(path(:7)=='/posts/'.and.(get.or.like_route)) then
        if(like_route) then
          id_text=path(8:len(path)-5)
        else
          id_text=path(8:)
        end if
      end if
    end if
    if(index(id_text,'/')/=0) id_text=''
    if(.not.create_route.and..not.feed_route.and.len(id_text)==0) then
      status=404; out=error_body('not found'); return
    end if
    if(submit) then
      call authenticate(auth,user,name,message)
      if(len(message)/=0) then
        status=401; out=error_body(message); return
      end if
    end if
    id=positive(id_text)
    if(.not.create_route.and..not.feed_route.and.id==0) then
      status=400; out=error_body('invalid post id'); return
    end if
    if(create_route) then
      if(.not.strict_json(input)) then
        status=400; out=error_body('malformed JSON body'); return
      end if
      call db_text(body_query,1,input,len(input))
      rc=sqlite3_step(body_query)
      body=''
      if(rc==100) then
        if(column(body_query,1)=='text') body=column(body_query,0)
      end if
      call db_reset(body_query)
      if(rc/=100) return
      call trim_body(body,count)
      if(count<=0) then
        status=400; out=error_body('body is required'); return
      else if(count>500) then
        status=400; out=error_body('body must be at most 500 characters'); return
      end if
      if(.not.begin_write()) return
      call db_integer(create,1,user)
      call db_text(create,2,body,len(body))
      rc=sqlite3_step(create)
      if(rc==100) then
        new_id=sqlite3_column_int64(create,0)
        created=column(create,1)
        rc=sqlite3_step(create)
      end if
      call db_reset(create)
      if(rc/=101) return
      status=201
      out='{"post":{"id":'//number(new_id)//',"body":'//quote(body)// &
        ',"created_at":'//quote(created)//',"author":'//quote(name)//',"like_count":0}}'
      return
    end if
    if(like_route) then
      if(.not.begin_write()) return
      call db_integer(like,1,user)
      call db_integer(like,2,id)
      rc=sqlite3_step(like)
      changed=sqlite3_changes(db)
      call db_reset(like)
      if(rc/=101) return
      if(changed==0) then
        call db_integer(exists,1,id)
        rc=sqlite3_step(exists)
        call db_reset(exists)
        if(rc==101) then
          status=404; out=error_body('post not found'); return
        end if
        if(rc/=100) return
      end if
      status=200; message='true'
      if(changed/=0) then
        status=201; message='false'
      end if
      out='{"liked":true,"already_liked":'//message//',"post_id":'//number(id)//'}'
      return
    end if
    statement=post; out='{"post":'
    if(feed_route) then
      statement=feed; out='{"posts":['
    else
      call db_integer(post,1,id)
    end if
    count=0
    do
      rc=sqlite3_step(statement)
      if(rc/=100) exit
      if(count>0) out=out//','
      out=out//row(statement)
      count=count+1
    end do
    call db_reset(statement)
    if(rc/=101) then
      out=error_body('internal server error'); return
    end if
    if(.not.feed_route.and.count==0) then
      status=404; out=error_body('post not found'); return
    end if
    if(feed_route) out=out//']'
    out=out//'}'; status=200
  end subroutine
  subroutine app_request(method,path,auth,body,n,connection) bind(C)
    type(c_ptr), value :: method,path,auth,body,connection
    integer(c_int), value :: n
    integer(c_int) :: status
    character(:), allocatable :: out
    call dispatch(cstring(method),cstring(path),cstring(auth),text(body,n),status,out)
    if(transaction) then
      if(sqlite3_get_autocommit(db)/=0) failed=.true.
    end if
    call http_reply(connection,status,out,len(out),merge(1,0,transaction))
  end subroutine
  integer(c_int) function app_flush() bind(C) result(ok)
    integer :: rc
    if(transaction) then
      if(.not.failed) failed=db_exec(db,'COMMIT'//c_null_char)/=0
      if(failed) then
        rc=db_exec(db,'ROLLBACK'//c_null_char)
        if(sqlite3_get_autocommit(db)==0) error stop 'unable to roll back failed batch'
      end if
    end if
    ok=merge(0,1,failed)
    transaction=.false.; failed=.false.
  end function
  function environment(key,fallback) result(value)
    character(*), intent(in) :: key,fallback
    character(:), allocatable :: value
    integer :: n,status
    call get_environment_variable(key,length=n,status=status)
    if(status/=0.or.n==0) then
      value=fallback
    else
      allocate(character(n) :: value)
      call get_environment_variable(key,value)
    end if
  end function
  subroutine initialize()
    character(:), allocatable :: path
    character(*), parameter :: read_sql='SELECT p.id,p.body,p.created_at,u.username,'// &
      '(SELECT count(*) FROM likes WHERE post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id'
    integer :: rc
    path=environment('SQLITE_PATH',''); secret=environment('JWT_SECRET','')
    if(len(path)==0.or.len(secret)==0) error stop 'SQLITE_PATH and JWT_SECRET are required'
    rc=db_open(path//c_null_char,db)
    if(rc/=0) error stop 'cannot open SQLite database'
    rc=db_exec(db,'PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;'// &
      'PRAGMA foreign_keys=ON; PRAGMA cache_size=-8192; PRAGMA mmap_size=268435456;'//c_null_char)
    if(rc/=0) error stop 'cannot configure SQLite database'
    feed=prepare(read_sql//' ORDER BY p.created_at DESC,p.id DESC LIMIT 20')
    post=prepare(read_sql//' WHERE p.id=?')
    create=prepare('INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at')
    like=prepare('INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS'// &
      '(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING')
    exists=prepare('SELECT 1 FROM posts WHERE id=?')
    health=prepare('SELECT 1')
    valid=prepare('SELECT json_valid(?)')
    body_query=prepare("SELECT json_extract(?1,'$.body'),json_type(?1,'$.body')")
    auth_query=prepare("SELECT json_extract(?2,'$.exp'),json_extract(?2,'$.nbf'),"// &
      "json_extract(?1,'$.alg')='HS256' AND json_type(?2,'$.exp') IN ('integer','real')"// &
      " AND (json_type(?2,'$.nbf') IS NULL OR json_type(?2,'$.nbf') IN ('integer','real')),"// &
      "json_type(?2,'$.sub')='text' AND json_type(?2,'$.username')='text',"// &
      "json_extract(?2,'$.sub'),json_extract(?2,'$.username')")
    started=monotonic_time()
  end subroutine
end module

program server
  use application
  implicit none
  character(:), allocatable :: host,port_text
  integer :: port,status
  call initialize()
  host=environment('HOST','127.0.0.1'); port_text=environment('PORT','3000')
  read(port_text,*,iostat=status) port
  if(status/=0) error stop 'invalid PORT'
  status=http_run(host//c_null_char,port)
  if(status/=0) error stop 'HTTP server failed'
end program
