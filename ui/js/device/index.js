// Picks the device implementation. Three live side by side:
//   "mock"    MockDevice: the simulator the app starts with (no hardware)
//   "webhid"  WebHidDevice: read-only version query, works with any firmware
//   "kmfw"    KmfwDevice: OUR firmware, configuration protocol (CONFIG_PROTOCOL.md), reading always, writing only when armed
// `probeFirmware(hid)` tells which of the last two a chosen keyboard needs, using only the cmd 3 version query.
import { MockDevice } from "./MockDevice.js";
import { WebHidDevice } from "./WebHidDevice.js";
import { KmfwDevice } from "./KmfwDevice.js";
import { WebHidTransport } from "./hid_transport.js";
import { buildInfoQuery, decodeBuildReply, isKmfwBuild } from "./config_protocol.js";

export const DEVICE_KINDS = Object.freeze(["mock", "webhid", "kmfw"]);

export function createDevice(kind, { layout, hidApi } = {}) {
  switch (kind) {
    case "mock": return new MockDevice(layout);
    case "webhid": return new WebHidDevice();
    case "kmfw": return new KmfwDevice({ hidApi });
    default: throw new RangeError(`unknown device kind '${kind}' (expected ${DEVICE_KINDS.join(", ")})`);
  }
}

/** "kmfw" when the keyboard's build string carries KMFW, "webhid" otherwise. Sends only the cmd 3 version query. */
export async function probeFirmware(hid) {
  if (!hid.opened) await hid.open();
  const { build } = decodeBuildReply(await new WebHidTransport(hid).exchange(buildInfoQuery()));
  return isKmfwBuild(build) ? "kmfw" : "webhid";
}
