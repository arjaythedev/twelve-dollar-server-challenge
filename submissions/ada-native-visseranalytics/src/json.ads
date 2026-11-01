package JSON is
   type Value_Kind is (Missing, String_Value, Number_Value, Object_Value,
                      Array_Value, Boolean_Value, Null_Value);
   -- A top-level member value located in the source document. String
   -- values exclude their quotes; Escaped says whether Decode must unescape.
   type Field is record
      Kind : Value_Kind := Missing;
      First : Positive := 1;
      Last : Natural := 0;
      Escaped : Boolean := False;
   end record;
   type Field_Array is array (Positive range <>) of Field;
   type Name_Access is access constant String;
   type Name_Array is array (Positive range <>) of Name_Access;
   Invalid_JSON : exception;

   -- Validate the whole document (RFC 8259, UTF-8) and locate the requested
   -- top-level members. Duplicate keys follow last-value-wins.
   procedure Parse (Source : String; Names : Name_Array;
                    Result : out Field_Array; Root : out Value_Kind)
     with Pre => Result'First = Names'First and then Result'Last = Names'Last;

   -- Unescaped bytes of a String_Value field. Target must hold at least
   -- F.Last - F.First + 1 bytes (unescaping never grows the text).
   procedure Decode (Source : String; F : Field; Target : in out String;
                     Length : out Natural);
   function Equals (Source : String; F : Field; Text : String) return Boolean;

   procedure Trim (Text : String; First : out Positive; Last : out Natural);
   function Character_Count (Text : String) return Natural;
   function Positive_ID (Text : String) return Long_Long_Integer;
end JSON;
