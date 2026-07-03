"""Diagnostics support for the Hable integration."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from homeassistant.core import HomeAssistant

from .const import CONF_ESPHOME_ENTRY_ID

if TYPE_CHECKING:
    from . import HableConfigEntry


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, config_entry: HableConfigEntry
) -> dict[str, Any]:
    """Return diagnostics for a config entry."""
    bridge = config_entry.runtime_data
    return {
        "entry": {
            "title": config_entry.title,
            "unique_id": config_entry.unique_id,
            "esphome_entry_id": config_entry.options.get(CONF_ESPHOME_ENTRY_ID),
        },
        "bridge": bridge.diagnostics(),
    }
