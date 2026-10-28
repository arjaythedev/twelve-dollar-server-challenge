with Ada.Strings.Unbounded;

package JSON is
   use Ada.Strings.Unbounded;
   type Value_Kind is (Missing, String_Value, Number_Value, Object_Value,
                      Array_Value, Boolean_Value, Null_Value);
   type Field is record
      Name : Unbounded_String;
      Kind : Value_Kind := Missing;
      Text : Unbounded_String;
   end record;
   type Fields is array (Positive range <>) of Field;
   Invalid_JSON : exception;

   -- Validate the whole document and extract only requested top-level fields.
   -- Duplicate object keys follow the usual last-value-wins convention.
   procedure Parse (Source : String; Result : in out Fields;
                    Root : out Value_Kind);
   function Quote (Text : String) return String;
   function Trim (Text : String) return String;
   function Character_Count (Text : String) return Natural;
   function Positive_ID (Text : String) return Long_Long_Integer;
   function Number (Value : Long_Long_Integer) return String;
end JSON;
