Import("env")
import shutil, os

def copy_firmware(source, target, env):
    src = str(target[0])
    dst = os.path.join(env.subst("$PROJECT_DIR"), "firmware.bin")
    shutil.copy(src, dst)
    print(f"[OTA] firmware.bin skopiowany do {dst}")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)
