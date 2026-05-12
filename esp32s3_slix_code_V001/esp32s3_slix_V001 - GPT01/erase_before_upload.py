Import("env")

def erase_flash(source, target, env):
    env.Execute(
        env.VerboseAction(
            "$PYTHONEXE $UPLOADER --chip esp32s3 --port $UPLOAD_PORT erase_flash",
            "Erasing flash..."
        )
    )

env.AddPreAction("upload", erase_flash)
