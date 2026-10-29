"""Small ESPHome API helper for test windows (run from the repository root with .venv/bin/python).

  dev/ofv_api.py get <name fragment>...        print the states whose entity name contains a fragment
  dev/ofv_api.py set <exact number name> <v>   set a number entity
  dev/ofv_api.py select <exact select name> <option>   choose a select option
  dev/ofv_api.py press <exact button name>     press a button entity
"""
import asyncio
import re
import sys

import aioesphomeapi

HOST = "10.35.5.18"
KEY = re.search(r'^api_key:\s*"?([^"\n]+)"?', open("dev/secrets.yaml").read(), re.M).group(1)


async def main():
    client = aioesphomeapi.APIClient(HOST, 6053, None, noise_psk=KEY)
    await client.connect(login=True)
    entities, _ = await client.list_entities_services()
    names = {e.key: e.name for e in entities}
    action = sys.argv[1]
    if action in ("set", "press", "select"):
        entity = next(e for e in entities if e.name == sys.argv[2])
        if action == "set":
            client.number_command(entity.key, float(sys.argv[3]))
        elif action == "select":
            client.select_command(entity.key, sys.argv[3])
        else:
            client.button_command(entity.key)
        await asyncio.sleep(1)
        print(action, entity.name, *sys.argv[3:])
    else:
        seen = {}
        client.subscribe_states(lambda s: seen.__setitem__(s.key, s))
        await asyncio.sleep(4)
        for key, state in seen.items():
            name = names.get(key, "?")
            if any(w.lower() in name.lower() for w in sys.argv[2:]):
                print(f"{name}: {getattr(state, 'state', '')}")
    await client.disconnect()


asyncio.run(main())
