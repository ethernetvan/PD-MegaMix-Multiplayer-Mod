import { describe, expect, it } from "vitest";
import { stripToken, tokenFromUrl } from "./auth";

describe("tokenFromUrl", () => {
  it("extracts the token", () => {
    expect(tokenFromUrl("?token=abc")).toBe("abc");
    expect(tokenFromUrl("?a=1&token=t2&b=3")).toBe("t2");
    expect(tokenFromUrl("?token=a%20b")).toBe("a b");
  });
  it("null when missing or empty", () => {
    expect(tokenFromUrl("")).toBeNull();
    expect(tokenFromUrl("?token=")).toBeNull();
    expect(tokenFromUrl("?other=1")).toBeNull();
  });
});

describe("stripToken", () => {
  it("removes only token", () => {
    expect(stripToken("?token=x&a=1")).toBe("?a=1");
    expect(stripToken("?a=1&token=x")).toBe("?a=1");
    expect(stripToken("?token=x")).toBe("");
    expect(stripToken("?a=1")).toBe("?a=1");
  });
});
