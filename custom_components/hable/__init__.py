"""The Hable (ESPHome API over BLE) integration.

Sets up a HableBridge (BLE <-> localhost TCP byte pump) per config entry and
makes sure a stock esphome config entry exists pointing at the bridge's
127.0.0.1:<port> socket. The esphome integration remains the entity layer;
hable is transport only (ARCHITECTURE.md section 4).
"""

from __future__ import annotations

import logging

from homeassistant.config_entries import SOURCE_USER, ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.exceptions import ConfigEntryNotReady

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
        # TODO: fallback matching. Without a stored entry_id there is no
        # reliable tag linking an esphome 127.0.0.1 entry back to our BLE
        # address (multiple hable bridges all live on 127.0.0.1 with ephemeral
        # ports). Consider matching via the esphome entry's unique_id (the
        # device MAC, which the device could derive from its BLE address) once
        # the device side is running.
        for candidate in hass.config_entries.async_entries(ESPHOME_DOMAIN):
            if candidate.data.get(CONF_HOST) == BRIDGE_HOST and candidate.data.get(
                "hable_address"
            ) == entry.data[CONF_ADDRESS]:
                esphome_entry = candidate
                break

    if esphome_entry is not None:
        _async_link_esphome_entry(hass, entry, esphome_entry.entry_id)
        if esphome_entry.data.get(CONF_PORT) != port:
            hass.config_entries.async_update_entry(
                esphome_entry,
                data={**esphome_entry.data, CONF_HOST: BRIDGE_HOST, CONF_PORT: port},
            )
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
        # section 5). The flow stays open in the UI for the user to finish.
        # TODO: after the user completes it we still need to learn the created
        # entry_id (e.g. rescan esphome entries for host 127.0.0.1 + our port
        # on next setup, or listen for config entry creation).
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
