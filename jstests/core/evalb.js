// Check the return value of a db.eval function running a database query, and ensure the function's
// contents are logged in the profile log.
//
// @tags: [
//   assumes_read_preference_unchanged,
//   creates_and_authenticates_user,
//   does_not_support_stepdowns,
//   requires_eval_command,
//   requires_non_retryable_commands,
//   requires_profiling
// ]

// Use a reserved database name to avoid a conflict in the parallel test suite.
var stddb = db;
var db = db.getSisterDB('evalb');

function profileCursor() {
    return db.system.profile.find({user: username + "@" + db.getName()});
}

function lastOp() {
    return profileCursor().sort({$natural: -1}).next();
}

try {
    username = 'jstests_evalb_user';
    db.dropUser(username);
    db.createUser({
        user: username,
        pwd: 'Password@a1b',
        roles: jsTest.basicUserRoles, "passwordDigestor": "server"
    });
    db.auth(username, 'Password@a1b');

    t = db.evalb;
    t.drop();

    t.save({x: 3});

    assert.eq(3,
              db.eval(function() {
                  return db.evalb.findOne().x;
              }),
              'A');

    db.setProfilingLevel(2);

    assert.eq(3,
              db.eval(function() {
                  return db.evalb.findOne().x;
              }),
              'B');

    o = lastOp();
    assert(tojson(o).indexOf('findOne().x') > 0, 'C : ' + tojson(o));
} finally {
    db.setProfilingLevel(0);
    db = stddb;
}
