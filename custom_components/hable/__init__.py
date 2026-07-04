"""The Hable (ESPHome API over BLE) integration.

Sets up a HableBridge (BLE <-> localhost TCP byte pump) per config entry and
makes sure a stock esphome config entry exists pointing at the bridge's
127.0.0.1:<port> socket. The esphome integration remains the entity layer;
hable is transport only (ARCHITECTURE.md section 4).
"""

from __future__ import annotations

import logging
from collections.abc import Callable

from homeassistant.config_entries import (
    SIGNAL_CONFIG_ENTRY_CHANGED,
    SOURCE_USER,
    ConfigEntry,
    ConfigEntryChange,
)
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant, callback
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers.device_registry import format_mac
from homeassistant.helpers.dispatcher import async_dispatcher_connect

from .bridge import HableBridge, HableBridgeError
from .const import BRIDGE_HOST, CONF_ADDRESS, CONF_ESPHOME_ENTRY_ID, CONF_NAME, DOMAIN

_LOGGER = logging.getLogger(__name__)

ESPHOME_DOMAIN = "esphome"

type HableConfigEntry = ConfigEntry[HableBridge]


async def async_setup_entry(hass: HomeAssistant, entry: HableConfigEntry) -> bool:
    """Set up hable from a config entry."""
    address: str = entry.data[CONF_ADDRESS]
    name: str = entry.data.get(CONF_NAME, address)

    bridge = HableBridge(hass, address, name)
    try:
        port = await bridge.async_start()
    except HableBridgeError as err:
        raise ConfigEntryNotReady(str(err)) from err

    entry.runtime_data = bridge

    try:
        await _async_ensure_esphome_entry(hass, entry, port)
    except Exception:  # noqa: BLE001 - the bridge is still useful without it
        _LOGGER.exception(
            "%s: failed to create/update the companion esphome entry", address
        )

    return True


async def async_unload_entry(hass: HomeAssistant, entry: HableConfigEntry) -> bool:
    """Unload a config entry (the esphome entry is left alone)."""
    await entry.runtime_data.async_stop()
    return True


async def _async_ensure_esphome_entry(
    hass: HomeAssistant, entry: HableConfigEntry, port: int
) -> None:
    """Ensure a stock esphome config entry points at 127.0.0.1:<port>.

    The bridge port is ephemeral, so on every setup the companion esphome
    entry's stored port must be rewritten (ARCHITECTURE.md section 8, risk 5).
    """
    # The esphome integration sets its entry unique_id to
    # format_mac(DeviceInfoResponse.mac_address); the firmware reports its BT
    # MAC, which equals the BLE address we store -- so format_mac(our address)
    # is the reliable cross-layer key linking an esphome entry to this bridge.
    our_mac = format_mac(entry.data[CONF_ADDRESS])

    esphome_entry = None
    if esphome_entry_id := entry.options.get(CONF_ESPHOME_ENTRY_ID):
        esphome_entry = hass.config_entries.async_get_entry(esphome_entry_id)
        if esphome_entry is None or esphome_entry.domain != ESPHOME_DOMAIN:
            _LOGGER.warning(
                "Stored esphome entry %s no longer exists; creating a new one",
                esphome_entry_id,
            )
            esphome_entry = None
    else:
        # No stored entry_id: match on the esphome entry's unique_id. Also
        # require host == 127.0.0.1 as belt-and-suspenders so we never adopt a
        # real (WiFi) esphome entry that happens to share the MAC.
        for candidate in hass.config_entries.async_entries(ESPHOME_DOMAIN):
            if (
                candidate.unique_id == our_mac
                and candidate.data.get(CONF_HOST) == BRIDGE_HOST
            ):
                esphome_entry = candidate
                break

    if esphome_entry is not None:
        _async_link_esphome_entry(hass, entry, esphome_entry.entry_id)
        _async_repoint_esphome_entry(hass, esphome_entry, port)
        await hass.config_entries.async_reload(esphome_entry.entry_id)
        return

    # No esphome entry yet: start the esphome user flow programmatically.
    # Verified against HA core (2026.7 dev): FlowManager.async_init passes
    # `data` to async_step_user as user_input, and the esphome user step
    # accepts {CONF_HOST: str, CONF_PORT: int} and immediately connects to
    # fetch device info -- so the bridge must already be running (it is).
    result = await hass.config_entries.flow.async_init(
        ESPHOME_DOMAIN,
        context={"source": SOURCE_USER},
        data={CONF_HOST: BRIDGE_HOST, CONF_PORT: port},
    )

    if result["type"] is FlowResultType.CREATE_ENTRY:
        _async_link_esphome_entry(hass, entry, result["result"].entry_id)
        return

    if result["type"] is FlowResultType.FORM:
        # The flow needs user input (typically the Noise encryption key, which
        # the esphome integration's own flow handles -- see ARCHITECTURE.md
        # section 5). The flow stays open in the UI for the user to finish, so
        # the created entry_id is not known yet: listen for the esphome entry
        # (matched by unique_id) to appear, then link and repoint it.
        _async_await_esphome_entry(hass, entry, our_mac, port)
        _LOGGER.warning(
            "%s: esphome flow requires user input (step %s); finish it in the UI",
            entry.data[CONF_ADDRESS],
            result.get("step_id"),
        )
        return

    _LOGGER.warning(
        "%s: unexpected esphome flow result: %s (reason=%s)",
        entry.data[CONF_ADDRESS],
        result["type"],
        result.get("reason"),
    )


def _async_link_esphome_entry(
    hass: HomeAssistant, entry: HableConfigEntry, esphome_entry_id: str
) -> None:
    """Persist the companion esphome entry_id in our entry options."""
    if entry.options.get(CONF_ESPHOME_ENTRY_ID) == esphome_entry_id:
        return
    hass.config_entries.async_update_entry(
        entry,
        options={**entry.options, CONF_ESPHOME_ENTRY_ID: esphome_entry_id},
    )


def _async_repoint_esphome_entry(
    hass: HomeAssistant, esphome_entry: ConfigEntry, port: int
) -> bool:
    """Rewrite the esphome entry's host/port to the bridge if stale.

    Returns True if the entry data was changed (so the caller should reload it).
    """
    if (
        esphome_entry.data.get(CONF_HOST) == BRIDGE_HOST
        and esphome_entry.data.get(CONF_PORT) == port
    ):
        return False
    hass.config_entries.async_update_entry(
        esphome_entry,
        data={**esphome_entry.data, CONF_HOST: BRIDGE_HOST, CONF_PORT: port},
    )
    return True


def _async_await_esphome_entry(
    hass: HomeAssistant, entry: HableConfigEntry, our_mac: str, port: int
) -> None:
    """Link the companion esphome entry once the user finishes its (Noise) flow.

    The programmatic flow returned a FORM (the user must enter the Noise key in
    the UI), so the created entry_id is not known yet. Subscribe to config entry
    changes and link the esphome entry when it appears, matched by unique_id ==
    our_mac (belt-and-suspenders: host must be 127.0.0.1 so we never adopt a
    WiFi esphome entry). One-shot: the listener unsubscribes itself on the first
    match, and async_on_unload guarantees it dies with the entry if the user
    never finishes.
    """
    unsub: Callable[[], None] | None = None
    linked = False

    @callback
    def _unsubscribe() -> None:
        nonlocal unsub
        if unsub is not None:
            unsub()
            unsub = None

    @callback
    def _handle_change(change: ConfigEntryChange, changed: ConfigEntry) -> None:
        nonlocal linked
        # ADDED normally carries the unique_id (the esphome flow sets it before
        # creating the entry); UPDATED is handled too in case that ever changes.
        if change not in (ConfigEntryChange.ADDED, ConfigEntryChange.UPDATED):
            return
        if (
            linked
            or changed.domain != ESPHOME_DOMAIN
            or changed.unique_id != our_mac
            or changed.data.get(CONF_HOST) != BRIDGE_HOST
        ):
            return
        linked = True
        _async_link_esphome_entry(hass, entry, changed.entry_id)
        # Repoint (and reload) only if the port is stale; a freshly created
        # entry already uses the current port, so this usually no-ops and avoids
        # reloading an entry that the flow just set up.
        if _async_repoint_esphome_entry(hass, changed, port):
            hass.async_create_task(
                hass.config_entries.async_reload(changed.entry_id)
            )
        _unsubscribe()

    unsub = async_dispatcher_connect(
        hass, SIGNAL_CONFIG_ENTRY_CHANGED, _handle_change
    )
    entry.async_on_unload(_unsubscribe)
