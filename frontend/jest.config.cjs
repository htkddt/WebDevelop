module.exports = {
  testEnvironment: 'jsdom',
  roots: ['<rootDir>/../tests/frontend'],
  moduleDirectories: ['node_modules', '<rootDir>/node_modules'],
  testMatch: ['<rootDir>/../tests/frontend/**/*.test.jsx'],
  setupFilesAfterEnv: ['<rootDir>/../tests/frontend/setupTests.js'],
  moduleNameMapper: {
    '\\.(css|less|scss)$': 'identity-obj-proxy',
    '\\.(png|jpg|jpeg|gif|svg)$': '<rootDir>/../tests/frontend/mocks/fileMock.js',
    '^\\.\\./config/api$': '<rootDir>/../tests/frontend/mocks/apiConfig.js',
  },
};
