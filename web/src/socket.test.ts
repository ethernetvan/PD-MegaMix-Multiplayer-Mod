import { describe, expect, it } from "vitest";
import { connStatus, retryAuthName } from "./api/socket";

describe("connStatus", () => {
  it("first attempt is connecting, retries are reconnecting", () => {
    expect(connStatus(false)).toBe("connecting");
    expect(connStatus(true)).toBe("reconnecting");
  });
});

describe("retryAuthName", () => {
  it("retries with name only on bad_token when a token was saved", () => {
    expect(retryAuthName({ ok: false, error: "bad_token" }, true)).toBe(true);
  });
  it("does not retry without a saved token or on other errors", () => {
    expect(retryAuthName({ ok: false, error: "bad_token" }, false)).toBe(false);
    expect(retryAuthName({ ok: false, error: "forbidden" }, true)).toBe(false);
    expect(retryAuthName({ ok: true }, true)).toBe(false);
  });
});
