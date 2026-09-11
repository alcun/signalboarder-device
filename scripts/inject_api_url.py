"""Bake the Signalboarder server URL in at build time.

`flash` exports SIGNALBOARDER_API_URL. Without it the firmware starts with an
empty server field and requires setup; it never opts into somebody else's bill.
"""
import os

Import("env")  # noqa: F821  (SCons injects this)

api_url = os.environ.get("SIGNALBOARDER_API_URL", "").rstrip("/")
if api_url:
    env.Append(  # noqa: F821
        CPPDEFINES=[
            ("SIGNALBOARDER_PROVISIONED_API_URL", env.StringifyMacro(api_url))  # noqa: F821
        ]
    )
    print(f"Signalboarder: baking server URL {api_url}")
else:
    print("Signalboarder: no server baked in; setup is required")
