# Retired: use grpc-asr

Development has moved to [grpc-asr on Forgejo](https://git.rokkon.com/ai-pipestream/grpc-asr)
and its [GitHub mirror](https://github.com/ai-pipestream/grpc-asr).
This repository is retained only as an archive. File issues and submit changes
to grpc-asr.

grpc-asr provides the existing ASR collector API and the native MediaService
and SpeechWorkerService on port 50055 in one server. It includes direct and
HTTP media ingestion, UUID-filtered subscriptions, bounded in-memory hydration,
speech-aware chunking, Whisper and native Parakeet transcription.

The prototype history is preserved at tag `prototype-final-b440304`, whose
commit is `b440304b304a016297763138d227b6a2987621be`. Its implementation was
consolidated into grpc-asr; this archived prototype is not a supported deployment.
