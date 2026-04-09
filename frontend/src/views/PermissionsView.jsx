import '../styles/Contents.css';
import '../styles/Permission.css';

export const PermissionsView = ({ roles, loading }) => {
  if (loading) {
    return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>Loading data from Backend ...</h1>;
  }

  return (
    <div className="permissions-container">
      <h1 style={{ fontSize: '5rem', color: '#ccc' }}>System decentralization</h1>
      <div className="permissions-role-list">
        {roles.map((role, index) => (
          <div key={index} className="permissions-role-item">
            <span className="permissions-role-dot"></span>
            <span className="permissions-role-name">{role}</span>
          </div>
        ))}
      </div>
    </div>
  );
};

export default PermissionsView;