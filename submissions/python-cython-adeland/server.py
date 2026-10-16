import os
import engine

if __name__ == '__main__':
    engine.serve(os.environ['SQLITE_PATH'].encode(), os.environ['JWT_SECRET'].encode(),
                 os.environ.get('HOST', '127.0.0.1').encode(), int(os.environ.get('PORT', '3000')))
