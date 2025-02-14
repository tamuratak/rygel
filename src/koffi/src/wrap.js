// Copyright 2023 Niels Martignène <niels.martignene@protonmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the “Software”), to deal in 
// the Software without restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
// Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

const util = require('util');

function wrap(native) {
    let obj = {
        ...native,

        call: util.deprecate((ptr, type, ...args) => {
            ptr = new native.Pointer(ptr, native.pointer(type));
            return ptr.call(...args);
        }, 'The koffi.call() function is deprecated, use pointer.call() method instead', 'KOFFI009'),
        address: util.deprecate(ptr => ptr?.address ?? null, 'The koffi.address() function is deprecated, use pointer.address property instead', 'KOFFI010'),

        resolve: util.deprecate(native.type, 'The koffi.resolve() function is deprecated, use koffi.type() instead', 'KOFFI010'),
        introspect: util.deprecate(native.type, 'The koffi.introspect() function is deprecated, use koffi.type() instead', 'KOFFI011')
    };

    return obj;
}

module.exports = { wrap };
