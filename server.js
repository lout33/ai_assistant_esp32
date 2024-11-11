const WebSocket = require('ws');
const fs = require('fs');
const wav = require('wav');
const OpenAI = require('openai');
const ffmpeg = require('fluent-ffmpeg');

const openai = new OpenAI({
    apiKey: 'sk-proj-xxxxxx'
});

// Add the resampleAudio function
async function resampleAudio(inputFile, outputFile) {
    return new Promise((resolve, reject) => {
        ffmpeg(inputFile)
            .toFormat('wav')
            .audioFrequency(44100)
            .audioChannels(1)
            .on('end', () => {
                console.log('Audio resampling completed');
                resolve();
            })
            .on('error', (err) => {
                console.error('Error during resampling:', err);
                reject(err);
            })
            .save(outputFile);
    });
}

// Add the AI processing function
async function processAudioWithAI(inputFile) {
    try {
        // 1. Transcribe audio
        const transcription = await openai.audio.transcriptions.create({
            file: fs.createReadStream(inputFile),
            model: "whisper-1",
        });
        console.log('Transcribed text:', transcription.text);

        // 2. Get ChatGPT response
        const completion = await openai.chat.completions.create({
            model: "gpt-4",
            messages: [
                { role: "system", content: "You are a helpful assistant." },
                { role: "user", content: transcription.text }
            ]
        });
        const response = completion.choices[0].message.content;
        console.log('ChatGPT Response:', response);

        // 3. Convert to speech
        const speechResponse = await openai.audio.speech.create({
            model: "tts-1-hd",
            voice: "alloy",
            input: response,
            response_format: "wav",
            speed: 1.0
        });

        // 4. Save and resample audio
        const tempOutputFile = inputFile.replace('.wav', '_temp_response.wav');
        const finalOutputFile = inputFile.replace('.wav', '_response_44k.wav');
        const buffer = Buffer.from(await speechResponse.arrayBuffer());
        fs.writeFileSync(tempOutputFile, buffer);
        
        await resampleAudio(tempOutputFile, finalOutputFile);
        fs.unlinkSync(tempOutputFile);
        
        return finalOutputFile;
    } catch (error) {
        console.error('AI processing error:', error);
        return null;
    }
}

const WS_PORT = 8888;
const WAV_HEADER_SIZE = 44;
const CHUNK_SIZE = 1024;

const wsServer = new WebSocket.Server({ port: WS_PORT });

let fileWriter = null;
let isFirstChunk = true;

wsServer.on('connection', function connection(ws) {
    console.log('Client connected');
    
    ws.on('message', (data) => {
        console.log('Received audio chunk, size:', data.length);
        
        if (isFirstChunk) {
            if (fileWriter) {
                fileWriter.end();
            }
            const fileName = `recording-${Date.now()}.wav`;
            console.log(`Creating new recording: ${fileName}`);
            fileWriter = new wav.FileWriter(fileName, {
                channels: 1,
                sampleRate: 44100,
                bitDepth: 16
            });
            isFirstChunk = false;
        }
        
        if (fileWriter) {
            fileWriter.write(Buffer.from(data));
        }
    });

    let silenceTimeout = null;
    ws.on('message', () => {
        clearTimeout(silenceTimeout);
        silenceTimeout = setTimeout(async () => {
            console.log('Silence detected, ending recording');
            isFirstChunk = true;
            if (fileWriter) {
                const currentFile = fileWriter.path;
                fileWriter.end();
                fileWriter = null;
                
                console.log('Processing audio with AI...');
                const responseFile = await processAudioWithAI(currentFile);
                
                if (responseFile) {
                    console.log('Starting response audio stream');
                    streamResponseAudio(ws, responseFile);
                }
            }
        }, 500);
    });

    ws.on('close', () => {
        console.log('Client disconnected');
        if (fileWriter) {
            fileWriter.end();
            fileWriter = null;
        }
    });
});

function streamResponseAudio(ws, filename) {
    console.log(`Opening ${filename} for streaming`);
    const audioStream = fs.createReadStream(filename, {
        highWaterMark: CHUNK_SIZE,
        start: WAV_HEADER_SIZE
    });
    
    let chunkCount = 0;
    audioStream.on('data', (chunk) => {
        if (ws.readyState === WebSocket.OPEN) {
            chunkCount++;
            console.log(`Sending chunk ${chunkCount}, size: ${chunk.length}`);
            ws.send(chunk, { binary: true });
        }
    });

    audioStream.on('end', () => {
        console.log(`Finished streaming audio file. Total chunks sent: ${chunkCount}`);
    });

    audioStream.on('error', (error) => {
        console.error('Error reading audio file:', error);
    });
}

console.log(`WebSocket server started on port ${WS_PORT}`);