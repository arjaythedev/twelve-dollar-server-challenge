module bindings
  use iso_c_binding
  implicit none
  interface
    function db_open(path,db) bind(C) result(r)
      import
      character(c_char) :: path(*)
      type(c_ptr) :: db
      integer(c_int) :: r
    end function
    function db_exec(db,sql) bind(C) result(r)
      import
      type(c_ptr), value :: db
      character(c_char) :: sql(*)
      integer(c_int) :: r
    end function
    function db_prepare(db,sql) bind(C) result(r)
      import
      type(c_ptr), value :: db
      character(c_char) :: sql(*)
      type(c_ptr) :: r
    end function
    subroutine db_text(s,idx,text,n) bind(C)
      import
      type(c_ptr), value :: s
      integer(c_int), value :: idx,n
      character(c_char) :: text(*)
    end subroutine
    subroutine db_integer(s,idx,value) bind(C)
      import
      type(c_ptr), value :: s
      integer(c_int), value :: idx
      integer(c_int64_t), value :: value
    end subroutine
    subroutine db_reset(s) bind(C)
      import
      type(c_ptr), value :: s
    end subroutine
    function sqlite3_step(s) bind(C) result(r)
      import
      type(c_ptr), value :: s
      integer(c_int) :: r
    end function
    function sqlite3_column_int64(s,idx) bind(C) result(r)
      import
      type(c_ptr), value :: s
      integer(c_int), value :: idx
      integer(c_int64_t) :: r
    end function
    function sqlite3_column_double(s,idx) bind(C) result(r)
      import
      type(c_ptr), value :: s
      integer(c_int), value :: idx
      real(c_double) :: r
    end function
    function sqlite3_column_text(s,idx) bind(C) result(r)
      import
      type(c_ptr), value :: s
      integer(c_int), value :: idx
      type(c_ptr) :: r
    end function
    function sqlite3_column_bytes(s,idx) bind(C) result(r)
      import
      type(c_ptr), value :: s
      integer(c_int), value :: idx
      integer(c_int) :: r
    end function
    function sqlite3_changes(db) bind(C) result(r)
      import
      type(c_ptr), value :: db
      integer(c_int) :: r
    end function
    function sqlite3_get_autocommit(db) bind(C) result(r)
      import
      type(c_ptr), value :: db
      integer(c_int) :: r
    end function
    function sqlite3_errmsg(db) bind(C) result(r)
      import
      type(c_ptr), value :: db
      type(c_ptr) :: r
    end function
    function strlen(ptr) bind(C) result(r)
      import
      type(c_ptr), value :: ptr
      integer(c_size_t) :: r
    end function
    function crypto_verify(secret,ns,message,nm,signature,ng) bind(C) result(r)
      import
      character(c_char) :: secret(*),message(*),signature(*)
      integer(c_int), value :: ns,nm,ng
      integer(c_int) :: r
    end function
    function base64_decode(encoded,n,decoded) bind(C) result(r)
      import
      character(c_char) :: encoded(*),decoded(*)
      integer(c_int), value :: n
      integer(c_int) :: r
    end function
    function wall_time() bind(C) result(r)
      import
      real(c_double) :: r
    end function
    function monotonic_time() bind(C) result(r)
      import
      real(c_double) :: r
    end function
    subroutine http_reply(r,status,body,n,defer) bind(C)
      import
      type(c_ptr), value :: r
      integer(c_int), value :: status,n,defer
      character(c_char) :: body(*)
    end subroutine
    function http_run(host,port) bind(C) result(r)
      import
      character(c_char) :: host(*)
      integer(c_int), value :: port
      integer(c_int) :: r
    end function
  end interface
contains
  function text(ptr,n) result(value)
    type(c_ptr), value :: ptr
    integer, intent(in) :: n
    character(:), allocatable :: value
    character(c_char), pointer :: bytes(:)
    integer :: i
    allocate(character(n) :: value)
    if (n == 0) return
    call c_f_pointer(ptr,bytes,[n])
    do i=1,n
      value(i:i)=bytes(i)
    end do
  end function
  function cstring(ptr) result(value)
    type(c_ptr), value :: ptr
    character(:), allocatable :: value
    value=text(ptr,int(strlen(ptr)))
  end function
  function column(s,i) result(value)
    type(c_ptr), value :: s
    integer, intent(in) :: i
    character(:), allocatable :: value
    value=text(sqlite3_column_text(s,i),sqlite3_column_bytes(s,i))
  end function
end module
