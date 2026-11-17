# Strands Frog

Turn a round Waveshare ESP32-S3 touchscreen into a pocket voice assistant. Hold the top button to ask a question and release it to see and hear the answer. Double-tap to start or stop a longer recording and read the timestamped transcript. The Frog connects directly to the cloud over Wi-Fi after setup.

## What you need

- **Waveshare ESP32-S3-Touch-AMOLED-1.75C**, a USB-C data cable, and 2.4 GHz Wi-Fi. The build targets the **1.75C** board. [Waveshare hardware docs](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C) · [Amazon US, with battery](https://www.amazon.com/dp/B0GGY19TF9)
- ESP-IDF **5.5.5** with ESP32-S3 support, `uv`, `zip`, and the AWS CLI. The commands below use Bash on macOS or Linux. [ESP-IDF setup](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/index.html)
- An AWS account and an OpenRouter API key. The device uses your AWS endpoint and your OpenRouter account.

```sh
git clone https://github.com/tmoreton/strands-frog.git
cd strands-frog
```

## 1. Deploy the voice endpoint

In one AWS Region, create a private S3 bucket and a Secrets Manager secret using the default Secrets Manager encryption key. Give the secret these two keys:

- `OPENROUTER_API_KEY` with your OpenRouter API key.
- `DEMO_TOKEN` with a device token you generate using `openssl rand -hex 32`. You’ll put the same token in the firmware in step 2.

From the repository root, run the commands below with your bucket name, secret ARN, and Region. Use a new S3 object key whenever you deploy a changed ZIP.

```sh
REGION=us-east-2
BUCKET='your-private-artifact-bucket'
SECRET_ARN='your-secret-arn'
ARTIFACT_KEY="lambda/voice-$(date -u +%Y%m%dT%H%M%SZ).zip"

bash cloud/package.sh
aws s3 cp cloud/function.zip "s3://$BUCKET/$ARTIFACT_KEY" --region "$REGION"
aws cloudformation deploy \
  --region "$REGION" --stack-name strands-frog \
  --template-file cloud/template.yaml --capabilities CAPABILITY_IAM \
  --parameter-overrides ArtifactBucket="$BUCKET" ArtifactKey="$ARTIFACT_KEY" SecretArn="$SECRET_ARN"
```

Copy the `VoiceUrl` output from the `strands-frog` stack.

```sh
aws cloudformation describe-stacks --region "$REGION" --stack-name strands-frog \
  --query 'Stacks[0].Outputs[?OutputKey==`VoiceUrl`].OutputValue' --output text
```

## 2. Build and flash the frog

Connect the device and set `PORT` to its USB serial port. Before the first flash, back up all 32 MB of the factory image and store the backup privately. The example port below is for macOS.

```sh
PORT=/dev/cu.usbmodem101
uvx --from 'esptool==5.6.0' esptool --chip esp32s3 --port "$PORT" \
  --before usb-reset read-flash 0 0x2000000 ../waveshare-factory.bin
cp firmware/main/cloud_settings.example.h firmware/main/cloud_settings.h
```

In `firmware/main/cloud_settings.h`, set `STRANDS_CLOUD_URL` to `VoiceUrl` and `STRANDS_DEVICE_TOKEN` to the same token in the AWS secret. Keep the header and built firmware private since the binary contains the device token.

```sh
. /path/to/esp-idf/export.sh
cd firmware
idf.py build
idf.py -p "$PORT" flash
```

For app updates after that first flash, use `idf.py -p "$PORT" app-flash` to write just the application.

To return to factory firmware, restore the backup from your own board. Run the command from the repository root.

```sh
uvx --from 'esptool==5.6.0' esptool --chip esp32s3 --port "$PORT" \
  --before usb-reset write-flash 0x0 ../waveshare-factory.bin
```

## 3. Join Wi-Fi and ask a question

1. While the frog shows **Wi-Fi setup**, connect it to your unlocked Mac with a USB-C data cable, or join the `Strands-Frog-…` network on your phone using the password shown on the frog. Stay on that network if your phone says there’s no internet.
2. Open `http://frog.local/` if the setup page doesn’t open automatically. If the name doesn’t resolve, use `http://192.168.7.1/` over USB or `http://192.168.4.1/` over Wi-Fi. Pick your 2.4 GHz network, enter its password, and tap **Connect**.
3. Wait for the frog to appear alone in the center of the screen and wave. Hold the top button, ask “How do wood frogs survive freezing in winter?”, then release. After **Listening** and **Thinking**, the answer appears under the frog.

If the frog returns to setup, use the password currently shown on the gadget and check the network credentials. To change networks, tap the lower button, swipe left twice to **Wi-Fi setup**, then tap **Open setup**. You can also hold the lower button for three seconds and release.

| Control | Action |
| --- | --- |
| Tap the idle screen | Send a star around the Astronaut helmet, or make the frog hop and wink in the other themes |
| Swipe left or right on the idle screen | Switch between Classic, Wizard, Garden, and Astronaut |
| Hold the top button, then release | Talk and send a question |
| Double-tap the top button | Start or stop a long transcript |
| Tap the top button | Deep sleep when idle. During a long recording, turn off the screen and keep recording. |
| Tap the lower button | Open Settings from the frog. Press it again to go back to the frog from any Settings page. |
| Double-tap the lower button while idle | Deep sleep |
| Tap either button with the screen off | Wake the screen. After deep sleep, wait for Wi-Fi to reconnect. |

Settings opens on **Battery**, which shows the charge percentage in large type. Swipe left for **Volume**, then swipe left again for **Wi-Fi setup**. Swipe right to go back one page. In **Volume**, choose 0%, 30%, 55%, or 80%. At 0%, the reply appears on screen and ElevenLabs is skipped.

## Long transcription

Double-tap the top button to start a long recording, then double-tap it again to stop. If the screen is off, wake it with one tap first. Recording also stops at the one-hour limit. After the last clips finish processing, swipe up to read the timestamped transcript and tap **Next** when there are more pages.

Long transcription sends short audio clips to Whisper for English transcription, then saves the text on the Frog. Unavailable clips are marked with their elapsed time.

## How it works

The ESP32-S3 records audio and sends a WAV over HTTPS to API Gateway. Lambda authenticates the device and calls Whisper through OpenRouter. For a short question, Strands Agents prompts DeepSeek and can send the reply to ElevenLabs for speech. The device displays the reply and animates the frog as it listens or speaks. Each question starts a fresh turn.

`firmware/` holds the ESP-IDF application, Wi-Fi portal, and frog artwork. `cloud/` holds the Lambda function and CloudFormation template. Run the cloud unit tests from the repository root.

```sh
uv run --python 3.12 python -m unittest discover -s cloud
```

The Wi-Fi portal includes the Strands Agents frog SVG. Its license and notice are in [firmware/assets](firmware/assets/README.md).
