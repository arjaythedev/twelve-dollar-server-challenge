module unicode
  use iso_c_binding
  implicit none
contains
  integer function scalar(s,pos,code) result(n)
    character(*), intent(in) :: s
    integer, intent(in) :: pos
    integer, intent(out) :: code
    integer :: first,j,c
    n=0
    code=0
    if (pos>len(s)) return
    first=iachar(s(pos:pos))
    select case(first)
    case(0:127); n=1; code=first
    case(194:223); n=2; code=iand(first,31)
    case(224:239); n=3; code=iand(first,15)
    case(240:244); n=4; code=iand(first,7)
    case default; return
    end select
    if (pos+n-1>len(s)) then
      n=0; return
    end if
    do j=1,n-1
      c=iachar(s(pos+j:pos+j))
      if (iand(c,192)/=128) then
        n=0; return
      end if
      code=ishft(code,6)+iand(c,63)
    end do
    if ((n==2.and.code<128).or.(n==3.and.code<2048).or.(n==4.and.code<65536).or. &
        (code>=55296.and.code<=57343).or.code>1114111) n=0
  end function
  logical function valid_utf8(s) result(ok)
    character(*), intent(in) :: s
    integer :: pos,n,code
    pos=1; ok=.false.
    do while(pos<=len(s))
      n=scalar(s,pos,code)
      if(n==0) return
      pos=pos+n
    end do
    ok=.true.
  end function
  logical function space(c)
    integer, intent(in) :: c
    space=(c>=9.and.c<=13).or.c==32.or.c==160.or.c==5760.or. &
      (c>=8192.and.c<=8202).or.c==8232.or.c==8233.or.c==8239.or.c==8287.or.c==12288.or.c==65279
  end function
  subroutine trim_body(s,count)
    character(:), allocatable, intent(inout) :: s
    integer, intent(out) :: count
    integer :: pos,n,c,first,last,seen
    pos=1; first=0; last=0; count=0; seen=0
    do while(pos<=len(s))
      n=scalar(s,pos,c)
      if(n==0) then
        count=-1; return
      end if
      if(.not.space(c)) then
        if(first==0) first=pos
        last=pos+n-1
        seen=seen+1
        count=seen
      else if(first/=0) then
        seen=seen+1
      end if
      pos=pos+n
    end do
    if(first==0) then
      s=''
    else
      s=s(first:last)
    end if
  end subroutine
  function quote(s) result(out)
    character(*), intent(in) :: s
    character(:), allocatable :: out
    character(*), parameter :: hex='0123456789abcdef'
    character :: back
    integer :: i,c,k
    back=achar(92)
    allocate(character(len(s)*6+2) :: out)
    out(1:1)='"'; k=2
    do i=1,len(s)
      c=iachar(s(i:i))
      if(c==34.or.c==92) then
        out(k:k+1)=back//s(i:i); k=k+2
      else if(c<32) then
        out(k:k+5)=back//'u00'//hex(c/16+1:c/16+1)//hex(mod(c,16)+1:mod(c,16)+1); k=k+6
      else
        out(k:k)=s(i:i); k=k+1
      end if
    end do
    out(k:k)='"'
    out=out(:k)
  end function
  function number(n) result(s)
    integer(c_int64_t), intent(in) :: n
    character(:), allocatable :: s
    character(32) :: buffer
    write(buffer,'(i0)') n
    s=trim(buffer)
  end function
  integer(c_int64_t) function positive(s) result(n)
    character(*), intent(in) :: s
    integer :: i,c
    n=0
    do i=1,len(s)
      c=iachar(s(i:i))-48
      if(c<0.or.c>9.or.n>900719925474099_c_int64_t) then
        n=0; return
      end if
      n=n*10+c
    end do
    if(n>9007199254740991_c_int64_t) n=0
  end function
end module
