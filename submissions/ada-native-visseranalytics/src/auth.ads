with Ada.Strings.Unbounded;

package Auth is
   type Status is (Valid, Missing_Bearer, Invalid_Token, Invalid_Payload);
   procedure Verify (Authorization, Secret : String; Result : out Status;
                     User_ID : out Long_Long_Integer;
                     Username : out Ada.Strings.Unbounded.Unbounded_String);
end Auth;
