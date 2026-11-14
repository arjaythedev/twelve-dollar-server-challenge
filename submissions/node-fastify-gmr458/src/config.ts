function required(name: string): string {
  const value = process.env[name];
  if (value === undefined || value === "") {
    console.error(`${name} is required`);
    process.exit(1);
  }
  return value;
}

export const SQLITE_PATH: string = required("SQLITE_PATH");
export const JWT_SECRET: Buffer = Buffer.from(required("JWT_SECRET"), "utf8");
export const HOST: string = process.env["HOST"] ?? "127.0.0.1";
export const PORT: number = Number(process.env["PORT"] ?? "3000");
