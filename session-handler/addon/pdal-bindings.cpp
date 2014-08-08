#include <node.h>
#include <v8.h>

#include "pdal-bindings.hpp"

using namespace v8;

namespace
{
    const std::size_t chunkSize = 65536;
}

Persistent<Function> PdalBindings::constructor;

PdalBindings::PdalBindings()
    : m_pdalSession()
    , m_readData()
{ }

PdalBindings::~PdalBindings()
{ }

void PdalBindings::init(v8::Handle<v8::Object> exports)
{
    // Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(construct);
    tpl->SetClassName(String::NewSymbol("PdalBindings"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    // Prototype
    tpl->PrototypeTemplate()->Set(String::NewSymbol("construct"),
        FunctionTemplate::New(construct)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("parse"),
        FunctionTemplate::New(parse)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("create"),
        FunctionTemplate::New(create)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("destroy"),
        FunctionTemplate::New(destroy)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("getNumPoints"),
        FunctionTemplate::New(getNumPoints)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("getSchema"),
        FunctionTemplate::New(getSchema)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("cancel"),
        FunctionTemplate::New(cancel)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("read"),
        FunctionTemplate::New(read)->GetFunction());

    constructor = Persistent<Function>::New(tpl->GetFunction());
    exports->Set(String::NewSymbol("PdalBindings"), constructor);
}

Handle<Value> PdalBindings::construct(const Arguments& args)
{
    HandleScope scope;

    if (args.IsConstructCall())
    {
        // Invoked as constructor with 'new'.
        PdalBindings* obj = new PdalBindings();
        obj->Wrap(args.This());
        return args.This();
    }
    else
    {
        // Invoked as a function, turn into construct call.
        return scope.Close(constructor->NewInstance());
    }
}

void PdalBindings::doInitialize(
        const Arguments& args,
        const bool execute)
{
    HandleScope scope;

    std::string errMsg("");

    if (args[0]->IsUndefined() || !args[0]->IsString())
        errMsg = "'pipeline' must be a string - args[0]";
    if (args[1]->IsUndefined() || !args[1]->IsFunction())
        // Fatal.
        throw std::runtime_error("Invalid callback supplied to 'create'");

    Persistent<Function> callback(
            Persistent<Function>::New(Local<Function>::Cast(args[1])));

    if (errMsg.size())
    {
        errorCallback(callback, errMsg);
        scope.Close(Undefined());
        return;
    }

    const std::string pipeline(*v8::String::Utf8Value(args[0]->ToString()));

    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());

    // Perform PdalSession construction here.  This way we can use a single
    // PdalBindings object to validate multiple pipelines.
    obj->m_pdalSession.reset(new PdalSession());

    // Store everything we'll need to perform initialization.
    uv_work_t* req(new uv_work_t);
    req->data = new CreateData(
            obj->m_pdalSession,
            pipeline,
            execute,
            callback);

    uv_queue_work(
        uv_default_loop(),
        req,
        (uv_work_cb)([](uv_work_t *req)->void {
            CreateData* createData(reinterpret_cast<CreateData*>(req->data));

            try
            {
                createData->pdalSession->initialize(
                    createData->pipeline,
                    createData->execute);
            }
            catch (const std::runtime_error& e)
            {
                createData->errMsg = e.what();
            }
            catch (const std::bad_alloc& ba)
            {
                createData->errMsg = "Memory allocation failed in CREATE";
            }
            catch (...)
            {
                createData->errMsg = "Unknown error";
            }
        }),
        (uv_after_work_cb)([](uv_work_t* req, int status)->void {
            HandleScope scope;

            CreateData* createData(reinterpret_cast<CreateData*>(req->data));

            // Output args.
            const unsigned argc = 1;
            Local<Value> argv[argc] =
                {
                    Local<Value>::New(String::New(
                            createData->errMsg.data(),
                            createData->errMsg.size()))
                };

            createData->callback->Call(
                Context::GetCurrent()->Global(), argc, argv);

            // Dispose of the persistent handle so the callback may be
            // garbage collected.
            createData->callback.Dispose();

            delete createData;
            delete req;
        }));

    scope.Close(Undefined());
}

Handle<Value> PdalBindings::create(const Arguments& args)
{
    HandleScope scope;
    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());
    obj->doInitialize(args);
    return scope.Close(Undefined());
}

Handle<Value> PdalBindings::parse(const Arguments& args)
{
    HandleScope scope;
    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());
    obj->doInitialize(args, false);

    // Release this session from memory now - we will need to reset the
    // session anyway in order to use it after this.
    obj->m_pdalSession.reset();

    return scope.Close(Undefined());
}

Handle<Value> PdalBindings::destroy(const Arguments& args)
{
    HandleScope scope;
    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());

    obj->m_pdalSession.reset();

    return scope.Close(Undefined());
}

Handle<Value> PdalBindings::getNumPoints(const Arguments& args)
{
    HandleScope scope;
    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());

    return scope.Close(Integer::New(obj->m_pdalSession->getNumPoints()));
}

Handle<Value> PdalBindings::getSchema(const Arguments& args)
{
    HandleScope scope;
    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());

    const std::string schema(obj->m_pdalSession->getSchema());

    return scope.Close(String::New(schema.data(), schema.size()));
}

Handle<Value> PdalBindings::read(const Arguments& args)
{
    HandleScope scope;

    std::string errMsg("");

    if (args[0]->IsUndefined() || !args[0]->IsString())
        errMsg = "'host' must be a string - args[0]";
    else if (args[1]->IsUndefined() || !args[1]->IsNumber())
        errMsg = "'port' must be a number - args[1]";
    else if (args[2]->IsUndefined() || !args[2]->IsNumber())
        errMsg = "'start' offset must be a number - args[2]";
    else if (args[3]->IsUndefined() || !args[3]->IsNumber())
        errMsg = "'count' must be a number - args[3]";
    else if (args[4]->IsUndefined() || !args[4]->IsFunction())
        // Fatal.
        throw std::runtime_error("Invalid callback supplied to 'read'");

    Persistent<Function> callback(
            Persistent<Function>::New(Local<Function>::Cast(args[4])));

    if (errMsg.size())
    {
        errorCallback(callback, errMsg);
        return scope.Close(Undefined());
    }

    // Input args.  Type validation is complete by this point.
    const std::string host(*v8::String::Utf8Value(args[0]->ToString()));
    const std::size_t port(args[1]->Uint32Value());
    const std::size_t start(args[2]->Uint32Value());
    const std::size_t count(args[3]->Uint32Value());

    // Provide access to m_pdalSession from within this static function.
    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());

    if (start >= obj->m_pdalSession->getNumPoints())
    {
        errorCallback(callback, "Invalid start offset in 'read' request");
        return scope.Close(Undefined());
    }
    else
    {
        // Store everything we'll need to perform the read.
        uv_work_t* readReq(new uv_work_t);

        obj->m_readData = new ReadData(
                obj->m_pdalSession,
                host,
                port,
                start,
                count,
                callback);

        // This structure is the owner of the buffered point data, and will
        // delete it after this transmission is complete.
        readReq->data = obj->m_readData;

        // Read points asynchronously.
        uv_queue_work(
            uv_default_loop(),
            readReq,
            (uv_work_cb)([](uv_work_t *readReq)->void {
                ReadData* readData(reinterpret_cast<ReadData*>(readReq->data));

                // Buffer the point data from PDAL.
                try
                {
                    // This call will new up the readData->data array.
                    readData->numPoints =
                        readData->pdalSession->read(
                            &readData->data,
                            readData->start,
                            readData->count);

                    readData->numBytes =
                        readData->numPoints *
                        readData->pdalSession->getStride();

                    // The BufferTransmitter must not delete readData->data,
                    // and we must take care not to do so elsewhere until the
                    // BufferTransmitter::transmit() operation is complete.
                    readData->bufferTransmitter.reset(
                        new BufferTransmitter(
                                readData->host,
                                readData->port,
                                readData->data,
                                readData->numBytes));
                }
                catch (const std::runtime_error& e)
                {
                    readData->errMsg = e.what();
                }
                catch (...)
                {
                    readData->errMsg = "Unknown error";
                }
            }),
            (uv_after_work_cb)([](uv_work_t* readReq, int status)->void {
                ReadData* readData(reinterpret_cast<ReadData*>(readReq->data));

                if (readData->errMsg.size())
                {
                    // Propagate the error back to the remote host.
                    errorCallback(readData->callback, readData->errMsg);

                    // Clean up since we won't be calling the async send code.
                    delete [] readData->data;
                    delete readData;
                    return;
                }

                HandleScope scope;

                const unsigned argc = 3;
                Local<Value> argv[argc] =
                    {
                        Local<Value>::New(Null()), // err
                        Local<Value>::New(Integer::New(readData->numPoints)),
                        Local<Value>::New(Integer::New(readData->numBytes))
                    };

                // Call the provided callback to return the status of the
                // data about to be streamed to the remote host.
                readData->callback->Call(
                    Context::GetCurrent()->Global(), argc, argv);

                // Dispose of the persistent handle so this callback may be
                // garbage collected.
                readData->callback.Dispose();

                // Create a token for the actual data transmission portion of
                // the read.
                uv_work_t* sendReq(new uv_work_t);
                sendReq->data = readData;

                // Now stream all the buffered point data to the remote host
                // asynchronously.
                uv_queue_work(
                    uv_default_loop(),
                    sendReq,
                    (uv_work_cb)([](uv_work_t *sendReq)->void {
                        ReadData* readData(
                            reinterpret_cast<ReadData*>(sendReq->data));

                        try
                        {
                            const std::size_t numBytes(readData->numBytes);
                            std::size_t offset(0);

                            while (offset < numBytes && !readData->cancel)
                            {
                                readData->bufferTransmitter->transmit(
                                    offset,
                                    std::min(chunkSize, numBytes - offset));

                                offset += chunkSize;
                            }

                            if (readData->cancel)
                                std::cout << "CANCELLED at (" <<
                                    offset << " / " << numBytes <<
                                    ") bytes" << std::endl;
                        }
                        catch (...)
                        {
                            std::cout <<
                                "Caught error transmitting buffer" <<
                                std::endl;
                        }
                    }),
                    (uv_after_work_cb)([](uv_work_t* sendReq, int status)->void {
                        ReadData* readData(
                            reinterpret_cast<ReadData*>(sendReq->data));

                        // Read and data transmission complete.  Clean
                        // everything up - readData->bufferTransmitter may
                        // no longer be used.
                        delete [] readData->data;
                        delete readData;
                        delete sendReq;
                    })
                );

                delete readReq;
            })
        );
    }

    return scope.Close(Undefined());
}

Handle<Value> PdalBindings::cancel(const Arguments& args)
{
    HandleScope scope;
    PdalBindings* obj = ObjectWrap::Unwrap<PdalBindings>(args.This());

    bool cancelled(false);

    // TODO Race condition.  Mutex here with the deletes in read().
    if (obj->m_readData)
    {
        obj->m_readData->cancel = true;
        cancelled = true;
        std::cout << "Cancelling..." << std::endl;
    }

    return scope.Close(Boolean::New(cancelled));
}

void PdalBindings::errorCallback(
        Persistent<Function> callback,
        std::string errMsg)
{
    HandleScope scope;

    const unsigned argc = 1;
    Local<Value> argv[argc] =
        {
            Local<Value>::New(String::New(errMsg.data(), errMsg.size()))
        };

    callback->Call(Context::GetCurrent()->Global(), argc, argv);

    // Dispose of the persistent handle so the callback may be garbage
    // collected.
    callback.Dispose();

    scope.Close(Undefined());
}

//////////////////////////////////////////////////////////////////////////////

void init(Handle<Object> exports)
{
    PdalBindings::init(exports);
}

NODE_MODULE(pdalBindings, init)
